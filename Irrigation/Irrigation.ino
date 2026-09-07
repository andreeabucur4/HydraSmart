#define SIMULATE 0      // 1 = date simulate pe placa ; 0 = date reale prin LoRa
#define USE_SD   0      // 1 = salveaza datele pe cardul microSD ; 0 = dezactivat
#define SD_CS_PIN 7
#include <SPI.h>
#include <Wire.h>
#include <U8g2lib.h>
#if !SIMULATE
  #include <LoRa.h>
#endif
#if USE_SD
  #include <SD.h>
#endif

U8G2_SH1107_SEEED_128X128_F_SW_I2C u8g2(U8G2_R0, /*clock=*/SCL, /*data=*/SDA, /*reset=*/U8X8_PIN_NONE);

#define NZONES 6
const int ZONE_PINS[NZONES][2] = {
  { A3, -1 },   // Zona 1 - 240V
  { A4, -1 },   // Zona 2 - 240V
  { A5, A6 },   // Zona 3 - 24V
  {  0,  1 },   // Zona 4 - 24V
  {  2,  3 },   // Zona 5 - 24V
  {  4,  5 },   // Zona 6 - 24V
};

// Daca releele de pe placa se comanda invers (se inchid pe LOW), pune false
#define RELAY_ACTIVE_HIGH true

#define POT_THRESHOLD A1
#define POT_TIME      A2

// Butoane de navigare pentru paginarea afisajului OLED
#define BTN_NEXT 13   // SW11 - pagina urmatoare
#define BTN_PREV 14   // SW12 - pagina anterioara
#define NUM_PAGES 3
#define LOW_BATTERY 3900             // sub aceasta valoare - baterie scazuta
int  page = 0;                       // 0 = Zone, 1 = Mediu, 2 = Diagnostic
int  lastNext = HIGH, lastPrev = HIGH;
unsigned long lastPacketMs = 0;      // momentul ultimului pachet (LoRa sau simulat)
bool gotAnyPacket = false;          

// Parametri reglati din potentiometre
int irrigation_treshold = 30;   // kPa
int irrigation_time     = 90;   // secunde

// Stare zone 
int  relayState[NZONES];
int  irrigating[NZONES];
unsigned long irrStart[NZONES];
int  zoneMode[NZONES];          // 0 = AUTO, 1 = MANUAL ON, 2 = MANUAL OFF (din web)
String inLine = "";

// Date per zona (primite prin LoRa sau generate in simulare) 
float soil[NZONES], soilTemp[NZONES], airTemp[NZONES], airHum[NZONES];
float light[NZONES], pressure[NZONES], voltage[NZONES], co2[NZONES];
bool  hasData[NZONES];          
bool  hasCo2[NZONES];           

unsigned long lastFast = 0;
const unsigned long FAST_INTERVAL = 1000;   // prag/timp + stare zone - des (1 s)
String received = "";

#if USE_SD
bool sdOk = false;              
#endif

#if SIMULATE
// Stare pentru generarea datelor simulate 
unsigned long lastSim = 0;
const unsigned long SIM_INTERVAL = 10000;   
float simPhase = 0;
float simAirTemp = 24.0, simAirHum = 55.0, simLight = 800.0;
float simPressure = 1013.0, simVoltage = 4100.0, simCo2 = 600.0;
#endif

// Comanda fizica a unei zone pe toate iesirile ei
void writeZone(int i, bool on) {
  bool level = RELAY_ACTIVE_HIGH ? on : !on;
  for (int k = 0; k < 2; k++) {
    int p = ZONE_PINS[i][k];
    if (p >= 0) digitalWrite(p, level ? HIGH : LOW);
  }
}

void setup() {
  Serial.begin(9600);
  for (int i = 0; i < NZONES; i++) {
    for (int k = 0; k < 2; k++) {
      int p = ZONE_PINS[i][k];
      if (p >= 0) pinMode(p, OUTPUT);
    }
    relayState[i] = 0; irrigating[i] = 0; irrStart[i] = 0; zoneMode[i] = 0;
    hasData[i] = false; hasCo2[i] = false;
    soil[i] = soilTemp[i] = airTemp[i] = airHum[i] = 0;
    light[i] = pressure[i] = voltage[i] = co2[i] = 0;
    writeZone(i, false);
  }

  analogReadResolution(10);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);

#if !SIMULATE
  // Initializare LoRa (doar in modul cu date reale)
  if (!LoRa.begin(868E6)) {
    Serial.println("Starting LoRa failed!");
  }
  LoRa.setTxPower(18);
  LoRa.setSpreadingFactor(12);
#endif

#if USE_SD
  // Initializare card SD 
  sdOk = SD.begin(SD_CS_PIN);
  if (!sdOk) Serial.println("Card SD indisponibil -> continui fara salvarea datelor.");
  else       Serial.println("Card SD detectat.");
#endif

#if SIMULATE
  randomSeed(analogRead(A0));
  for (int i = 0; i < NZONES; i++) {           
    soil[i]     = 20.0 + (i * 5) % 30;
    soilTemp[i] = 18.0 + (i % 3) * 0.5;
  }
#endif
  u8g2.begin();
}

void loop() {
  readSerialCommands();   
  readPotentiometers();   
#if SIMULATE
  updateSimulation();     
#else
  readLoRa();             
#endif
  controlZones();         
  readButtons();          
  updateDisplay();        
  sendFast();             
  delay(50);
}

// Potentiometre 
void readPotentiometers() {
  int raw1 = analogRead(POT_THRESHOLD);
  int v1 = constrain(raw1, 520, 1020);
  int step1 = (v1 - 520) / 25; if (step1 < 0) step1 = 0; if (step1 > 20) step1 = 20;
  irrigation_treshold = 10 + step1 * 2;           // 10..50 kPa
  int raw2 = analogRead(POT_TIME);
  int v2 = constrain(raw2, 520, 1020);
  int step2 = (v2 - 520) / 50; if (step2 < 0) step2 = 0; if (step2 > 10) step2 = 10;
  irrigation_time = 30 * (step2 + 1);             // 30..330 s
}

#if !SIMULATE
// Receptie LoRa (date reale de la nodul senzor) 
void readLoRa() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;
  received = "";
  while (LoRa.available()) received += (char)LoRa.read();
  handlePacket(received);
}
#endif

bool handlePacket(String s) {
  s.trim();
  float v[9];
  int count = 0;
  int i = 0, len = s.length();
  while (i < len && count < 9) {
    while (i < len && s.charAt(i) == ' ') i++;          // sare peste spatii
    if (i >= len) break;
    int j = i;
    while (j < len && s.charAt(j) != ' ') j++;          // pana la urmatorul spatiu
    v[count++] = s.substring(i, j).toFloat();
    i = j;
  }
  if (count < 8) return false;                          // minim ID + 7 valori
  int id = (int)v[0];
  if (id < 1 || id > NZONES) return false;
  int z = id - 1;
  soil[z] = v[1]; soilTemp[z] = v[2]; airTemp[z] = v[3]; airHum[z] = v[4];
  light[z] = v[5]; pressure[z] = v[6]; voltage[z] = v[7];
  if (count >= 9) { co2[z] = v[8]; hasCo2[z] = true; }  // CO2 e optional
  hasData[z] = true;
  lastPacketMs = millis(); gotAnyPacket = true;   // pentru pagina de diagnostic
  sendDataLine(z);   // transmite imediat valorile catre web
#if USE_SD
  logToSD(z);        // salveaza pe card (daca este disponibil)
#endif
  return true;
}

#if SIMULATE
// Generarea datelor simulate 
void updateSimulation() {
  if (millis() - lastSim < SIM_INTERVAL) return;
  lastSim = millis();
  simPhase += 0.05;
  // Parametri de mediu (comuni tuturor zonelor)
  simLight = 800.0 + 600.0 * sin(simPhase / 2.0); if (simLight < 0) simLight = 0;
  simAirTemp = 19.0 + (simLight / 1500.0) * 8.0 + random(-3, 3) / 10.0;
  simAirHum  = 55.0 + 8.0 * sin(simPhase + 1.0) + random(-10, 10) / 10.0;
  simPressure = 1013.0 + random(-15, 15) / 10.0;
  simCo2 = 600.0 + 250.0 * sin(simPhase / 1.5) + random(-20, 20);
  if (simCo2 < 350) simCo2 = 350;
  simVoltage -= random(8, 16);
  if (simVoltage < 3820) simVoltage = 4150;
  simVoltage = constrain(simVoltage, 3700, 4150);
  for (int i = 0; i < NZONES; i++) {
    // Umiditatea solului evolueaza in functie de starea releului
    float s = soil[i];
    if (relayState[i]) s -= random(20, 40) / 10.0;       // se uda - tensiune scade
    else               s += random(4, 12) / 10.0;        // se usuca - tensiune creste
    s = constrain(s, 5.0, 80.0);
    // Temperatura solului depinde de umiditate (umed - mai rece, uscat - mai cald)
    float st = 16.0 + (s - 5.0) / 75.0 * 10.0 + random(-5, 5) / 10.0;
    st = constrain(st, 10.0, 30.0);

    String line = String(i + 1) + " " + String(s, 1) + " " + String(st, 1) + " " +
                  String(simAirTemp, 1) + " " + String(simAirHum, 1) + " " +
                  String(simLight, 1) + " " + String(simPressure, 1) + " " +
                  String((int)simVoltage) + " " + String((int)simCo2);
    handlePacket(line);
  }
}
#endif

void controlZones() {
  for (int i = 0; i < NZONES; i++) {
    if (zoneMode[i] == 1) {              // MANUAL ON (din web)
      relayState[i] = 1; irrigating[i] = 0;
    } else if (zoneMode[i] == 2) {       // MANUAL OFF (din web)
      relayState[i] = 0; irrigating[i] = 0;
    } else {                             // AUTO (prag + durata de udare)
      if (!hasData[i]) {                 // fara senzor pe zona - oprit in auto
        relayState[i] = 0; irrigating[i] = 0;
      } else if (soil[i] < irrigation_treshold) {  // sol umed - oprire
        relayState[i] = 0; irrigating[i] = 0;
      } else {                                     // sol uscat - uda
        if (!irrigating[i]) { irrigating[i] = 1; irrStart[i] = millis(); }
        // la expirarea timpului de udare zona se opreste scurt (sfarsit ciclu), apoi reporneste daca solul e inca uscat
        if (millis() - irrStart[i] >= (unsigned long)irrigation_time * 1000UL) {
          relayState[i] = 0; irrigating[i] = 0;
        } else {
          relayState[i] = 1;
        }
      }
    }
    writeZone(i, relayState[i]);
  }
}

void readButtons() {
  int n = digitalRead(BTN_NEXT);
  int p = digitalRead(BTN_PREV);
  if (n == LOW && lastNext == HIGH) page = (page + 1) % NUM_PAGES;              // pagina urmatoare
  if (p == LOW && lastPrev == HIGH) page = (page + NUM_PAGES - 1) % NUM_PAGES;  // pagina anterioara
  lastNext = n; lastPrev = p;
}

void updateDisplay() {
  char pg[6];
  u8g2.clearBuffer();
  if (page == 0)      drawPaginaZone();
  else if (page == 1) drawPaginaMediu();
  else                drawPaginaDiag();
  // indicator de pagina in coltul dreapta-jos (ex. "1/3")
  snprintf(pg, sizeof(pg), "%d/%d", page + 1, NUM_PAGES);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(112, 126, pg);
  u8g2.sendBuffer();
}

void drawPaginaZone() {
  char buf[22];
  u8g2.setFont(u8g2_font_9x15B_tr);
  u8g2.drawStr(2, 15, "IRIGATIE");
  u8g2.setFont(u8g2_font_6x10_tf);
  snprintf(buf, sizeof(buf), "Prag %2dkPa Timp%3ds", irrigation_treshold, irrigation_time);
  u8g2.drawStr(2, 29, buf);
  u8g2.drawStr(2, 43, "Zona  Sol   Releu");
  u8g2.setFont(u8g2_font_7x13_tf);
  for (int i = 0; i < NZONES; i++) {
    if (hasData[i])
      snprintf(buf, sizeof(buf), "Z%d  %2dkPa  %s", i + 1, (int)soil[i], relayState[i] ? "ON" : "..");
    else
      snprintf(buf, sizeof(buf), "Z%d   --    %s", i + 1, relayState[i] ? "ON" : "..");
    u8g2.drawStr(2, 57 + i * 12, buf);
  }
}

void drawPaginaMediu() {
  char buf[24];
  u8g2.setFont(u8g2_font_9x15B_tr);
  u8g2.drawStr(2, 15, "MEDIU");
  // Alege prima zona care a primit date (valorile de mediu sunt comune)
  int z = -1;
  for (int i = 0; i < NZONES; i++) if (hasData[i]) { z = i; break; }
  u8g2.setFont(u8g2_font_6x10_tf);
  if (z < 0) {
    u8g2.drawStr(2, 40, "Fara date inca...");
    return;
  }
  int y = 32;
  snprintf(buf, sizeof(buf), "Temp aer:  %d C",   (int)airTemp[z]);  u8g2.drawStr(2, y, buf); y += 16;
  snprintf(buf, sizeof(buf), "Umid aer:  %d %%",  (int)airHum[z]);   u8g2.drawStr(2, y, buf); y += 16;
  snprintf(buf, sizeof(buf), "Lumina:    %d lx",  (int)light[z]);    u8g2.drawStr(2, y, buf); y += 16;
  snprintf(buf, sizeof(buf), "Presiune:  %d hPa", (int)pressure[z]); u8g2.drawStr(2, y, buf); y += 16;
  if (hasCo2[z]) {
    snprintf(buf, sizeof(buf), "CO2:       %d ppm", (int)co2[z]);    u8g2.drawStr(2, y, buf);
  }
}

void drawPaginaDiag() {
  char buf[24];
  u8g2.setFont(u8g2_font_9x15B_tr);
  u8g2.drawStr(2, 15, "DIAGNOSTIC");
  unsigned long s = millis() / 1000;
  int onCount = 0, manCount = 0, z = -1;
  for (int i = 0; i < NZONES; i++) {
    if (relayState[i]) onCount++;
    if (zoneMode[i] != 0) manCount++;
    if (z < 0 && hasData[i]) z = i;
  }
  u8g2.setFont(u8g2_font_6x10_tf);
  int y = 36;
#if SIMULATE
  u8g2.drawStr(2, y, "Mod:        SIMULARE");
#else
  // Starea legaturii LoRa
  if (!gotAnyPacket) {
    u8g2.drawStr(2, y, "LoRa: fara pachete");
  } else {
    unsigned long ago = (millis() - lastPacketMs) / 1000;
    if (ago > 120) u8g2.drawStr(2, y, "LoRa: fara semnal");
    else { snprintf(buf, sizeof(buf), "LoRa: ultim %lus", ago); u8g2.drawStr(2, y, buf); }
  }
#endif
  y += 18;
  snprintf(buf, sizeof(buf), "Timp activ: %lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
  u8g2.drawStr(2, y, buf); y += 18;
  snprintf(buf, sizeof(buf), "Zone ON:    %d / %d", onCount, NZONES);  u8g2.drawStr(2, y, buf); y += 18;
  snprintf(buf, sizeof(buf), "Manual:     %d / %d", manCount, NZONES); u8g2.drawStr(2, y, buf); y += 18;
  if (z < 0) u8g2.drawStr(2, y, "Bat: --");
  else {
    snprintf(buf, sizeof(buf), "Bat: %d %s", (int)voltage[z], voltage[z] < LOW_BATTERY ? "SCAZUTA" : "OK");
    u8g2.drawStr(2, y, buf);
  }
}

void sendDataLine(int z) {
  Serial.print(z + 1);            Serial.print(' ');
  Serial.print(soil[z], 1);       Serial.print(' ');
  Serial.print(soilTemp[z], 1);   Serial.print(' ');
  Serial.print(airTemp[z], 1);    Serial.print(' ');
  Serial.print(airHum[z], 1);     Serial.print(' ');
  Serial.print(light[z], 1);      Serial.print(' ');
  Serial.print(pressure[z], 1);   Serial.print(' ');
  Serial.print((int)voltage[z]);
  if (hasCo2[z]) { Serial.print(' '); Serial.print((int)co2[z]); }  
  Serial.println();
}

void sendFast() {
  if (millis() - lastFast < FAST_INTERVAL) return;
  lastFast = millis();
  for (int i = 0; i < NZONES; i++) {
    Serial.print(relayState[i] ? "ON" : "OFF");
    Serial.println(i + 1);
    Serial.print("ZMODE ");
    Serial.print(i + 1);
    Serial.print(' ');
    Serial.println(zoneMode[i] == 0 ? "auto" : (zoneMode[i] == 1 ? "manon" : "manoff"));
  }
  Serial.print("analogValue1 = ");
  Serial.print(analogRead(POT_THRESHOLD));
  Serial.print(" => irrigation_treshold = ");
  Serial.println(irrigation_treshold);
  Serial.print("analogValue2 = ");
  Serial.print(analogRead(POT_TIME));
  Serial.print(" => irrigation_time = ");
  Serial.println(irrigation_time);
}

#if USE_SD
// Salvarea datelor pe cardul microSD 
void logToSD(int z) {
  if (!sdOk) return;                         
  File f = SD.open("irriglog.txt", FILE_WRITE);
  if (!f) return;
  f.print(millis());        f.print('\t');
  f.print(z + 1);           f.print('\t');
  f.print(soil[z], 1);      f.print('\t');
  f.print(soilTemp[z], 1);  f.print('\t');
  f.print(airTemp[z], 1);   f.print('\t');
  f.print(airHum[z], 1);    f.print('\t');
  f.print(light[z], 1);     f.print('\t');
  f.print(pressure[z], 1);  f.print('\t');
  f.print((int)voltage[z]); f.print('\t');
  f.println(relayState[z]);
  f.close();
}
#endif

void readSerialCommands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inLine.length() > 0) { handleCommand(inLine); inLine = ""; }
    } else {
      inLine += c;
      if (inLine.length() > 60) inLine = "";   
    }
  }
}

void handleCommand(String s) {
  s.trim();
  if (s.length() && s.charAt(0) >= '0' && s.charAt(0) <= '9') {
    handlePacket(s);
    return;
  }
  s.toUpperCase();
  if (!s.startsWith("Z")) return;
  int sp = s.indexOf(' ');
  if (sp < 2) return;
  int zn = s.substring(1, sp).toInt();
  String cmd = s.substring(sp + 1);
  if (zn < 1 || zn > NZONES) return;
  if (cmd == "ON")        zoneMode[zn - 1] = 1;
  else if (cmd == "OFF")  zoneMode[zn - 1] = 2;
  else if (cmd == "AUTO") zoneMode[zn - 1] = 0;
}