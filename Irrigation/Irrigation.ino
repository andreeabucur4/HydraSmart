#include <Wire.h>
#include <U8g2lib.h>

// Constructor OLED (varianta SEEED - aliniaza corect display-ul folosit)
U8G2_SH1107_SEEED_128X128_F_SW_I2C u8g2(U8G2_R0, /*clock=*/SCL, /*data=*/SDA, /*reset=*/U8X8_PIN_NONE);

// Zonele si pinii lor (zonele de 24V au cate 2 LED-uri; -1 = neutilizat)
#define NZONES 6
const int  ZONE_PINS[NZONES][2] = {
  { A3, -1 },   // Zona 1 - 240V
  { A4, -1 },   // Zona 2 - 240V
  { A5, A6 },   // Zona 3 - 24V
  {  0,  1 },   // Zona 4 - 24V
  {  2,  3 },   // Zona 5 - 24V
  {  4,  5 },   // Zona 6 - 24V
};

#define POT_THRESHOLD A1
#define POT_TIME      A2

// Butoane de navigare pentru paginarea afisajului OLED
#define BTN_NEXT 13   // SW11 - pagina urmatoare
#define BTN_PREV 14   // SW12 - pagina anterioara
#define NUM_PAGES 3
#define LOW_BATTERY 3900             // sub aceasta valoare bateria este scazuta
int  page = 0;                       // 0 = Zone, 1 = Mediu, 2 = Diagnostic
int  lastNext = HIGH, lastPrev = HIGH;

// Parametri reglati din potentiometre
int irrigation_treshold = 30;   // kPa
int irrigation_time     = 90;   // secunde

// Stare zone 
int  relayState[NZONES];
int  irrigating[NZONES];
unsigned long irrStart[NZONES];
int  zoneMode[NZONES];          // 0 = AUTO, 1 = MANUAL ON, 2 = MANUAL OFF
String inLine = "";

// Date SIMULATE (umiditate sol = tensiune kPa)
float soil[NZONES];
float soilTemp[NZONES];
float airTemp = 24.0, airHum = 55.0, light = 800.0, pressure = 1013.0, voltage = 4100.0, co2 = 600.0;

unsigned long lastSim = 0, lastFast = 0;
const unsigned long SIM_INTERVAL  = 10000;  // valorile simulate se schimba la ~10 s
const unsigned long FAST_INTERVAL = 1000;   // prag/timp + stare zone se schimba des (1 s)
float simPhase = 0;

// Scrie starea unei zone pe toate LED-urile/releele ei
void writeZone(int i, bool on) {
  for (int k = 0; k < 2; k++) {
    int p = ZONE_PINS[i][k];
    if (p >= 0) digitalWrite(p, on ? HIGH : LOW);
  }
}

void setup() {
  Serial.begin(9600);

  for (int i = 0; i < NZONES; i++) {
    for (int k = 0; k < 2; k++) {
      int p = ZONE_PINS[i][k];
      if (p >= 0) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
    }
    relayState[i] = 0; irrigating[i] = 0; irrStart[i] = 0; zoneMode[i] = 0;
    soil[i]     = 20.0 + (i * 5) % 30;
    soilTemp[i] = 18.0 + (i % 3) * 0.5;
  }

  analogReadResolution(10);
  randomSeed(analogRead(A0));
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);
  u8g2.begin();

  sendData();   // valorile initiale, pentru ca interfata web sa le aiba din start (la fel ca OLED-ul)
}

void loop() {
  readSerialCommands();
  readPotentiometers();
  updateSimulation();
  controlZones();
  readButtons();   // navigare pagini OLED (SW11/SW12)
  updateDisplay();
  sendFast();    // prag/timp + stare zone -> des (web sincron cu OLED)
  // sendData() se apeleaza din updateSimulation, fix cand se schimba valorile
  delay(100);
}

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

void updateSimulation() {
  if (millis() - lastSim < SIM_INTERVAL) return;
  lastSim = millis();
  simPhase += 0.05;

  for (int i = 0; i < NZONES; i++) {
    if (relayState[i]) soil[i] -= random(20, 40) / 10.0;
    else soil[i] += random(4, 12) / 10.0;
    soil[i] = constrain(soil[i], 5.0, 80.0);
    // Temperatura solului depinde de umiditate: sol umed (tensiune mica) -> mai rece; sol uscat (tensiune mare) -> mai cald (5..80 kPa -> ~16..26 C)
    soilTemp[i] = 16.0 + (soil[i] - 5.0) / 75.0 * 10.0 + random(-5, 5) / 10.0;
    soilTemp[i] = constrain(soilTemp[i], 10.0, 30.0);
  }

  light   = 800.0 + 600.0 * sin(simPhase / 2.0); if (light < 0) light = 0;
  // Mai multa lumina -> aer mai cald (corelatie lumina <-> temperatura)
  airTemp = 19.0 + (light / 1500.0) * 8.0 + random(-3, 3) / 10.0;
  airHum  = 55.0 + 8.0 * sin(simPhase + 1.0) + random(-10, 10) / 10.0;
  pressure = 1013.0 + random(-15, 15) / 10.0;
  co2  = 600.0 + 250.0 * sin(simPhase / 1.5) + random(-20, 20);
  if (co2 < 350) co2 = 350;
  // Baterie simulata: se descarca lent si se reincarca cand ajunge jos (panou solar), astfel incat sa coboare periodic sub prag -> alerta apare ocazional
  voltage -= random(8, 16);
  if (voltage < 3820) voltage = 4150;
  voltage = constrain(voltage, 3700, 4150);

  sendData();   // trimite noile valori imediat -> web sincron cu OLED
}

void controlZones() {
  for (int i = 0; i < NZONES; i++) {
    if (zoneMode[i] == 1) {              // MANUAL ON
      relayState[i] = 1; irrigating[i] = 0;
    } else if (zoneMode[i] == 2) {       // MANUAL OFF
      relayState[i] = 0; irrigating[i] = 0;
    } else {                             // AUTO (prag + durata de udare)
      if (soil[i] < irrigation_treshold) {       // sol umed -> oprire
        relayState[i] = 0; irrigating[i] = 0;
      } else {                                    // sol uscat -> uda
        if (!irrigating[i]) { irrigating[i] = 1; irrStart[i] = millis(); }
        // la expirarea timpului de udare, zona se opreste scurt (semnaleaza sfarsitul ciclului), apoi reporneste daca solul e inca uscat
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

// Citirea butoanelor de navigare (paginare OLED)
void readButtons() {
  int n = digitalRead(BTN_NEXT);
  int p = digitalRead(BTN_PREV);
  if (n == LOW && lastNext == HIGH) page = (page + 1) % NUM_PAGES;              // pagina urmatoare
  if (p == LOW && lastPrev == HIGH) page = (page + NUM_PAGES - 1) % NUM_PAGES;  // pagina anterioara
  lastNext = n; lastPrev = p;
}

// Afisare pe OLED
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
    snprintf(buf, sizeof(buf), "Z%d  %2dkPa  %s",
             i + 1, (int)soil[i], relayState[i] ? "ON" : "..");
    u8g2.drawStr(2, 57 + i * 12, buf);
  }
}

void drawPaginaMediu() {
  char buf[24];
  u8g2.setFont(u8g2_font_9x15B_tr);
  u8g2.drawStr(2, 15, "MEDIU");

  u8g2.setFont(u8g2_font_6x10_tf);
  int y = 36;
  snprintf(buf, sizeof(buf), "Temp. aer: %d C",   (int)airTemp);  u8g2.drawStr(2, y, buf); y += 18;
  snprintf(buf, sizeof(buf), "Umid. aer: %d %%",  (int)airHum);   u8g2.drawStr(2, y, buf); y += 18;
  snprintf(buf, sizeof(buf), "Lumina:    %d lx",  (int)light);    u8g2.drawStr(2, y, buf); y += 18;
  snprintf(buf, sizeof(buf), "Presiune:  %d hPa", (int)pressure); u8g2.drawStr(2, y, buf); y += 18;
  snprintf(buf, sizeof(buf), "CO2:       %d ppm", (int)co2);      u8g2.drawStr(2, y, buf);
}

void drawPaginaDiag() {
  char buf[24];
  u8g2.setFont(u8g2_font_9x15B_tr);
  u8g2.drawStr(2, 15, "DIAGNOSTIC");

  unsigned long s = millis() / 1000;
  int onCount = 0, manCount = 0;
  for (int i = 0; i < NZONES; i++) {
    if (relayState[i]) onCount++;
    if (zoneMode[i] != 0) manCount++;
  }

  u8g2.setFont(u8g2_font_6x10_tf);
  int y = 36;
  u8g2.drawStr(2, y, "Mod:        SIMULARE"); y += 18;
  snprintf(buf, sizeof(buf), "Timp activ: %lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
  u8g2.drawStr(2, y, buf); y += 18;
  snprintf(buf, sizeof(buf), "Zone ON:    %d / %d", onCount, NZONES);  u8g2.drawStr(2, y, buf); y += 18;
  snprintf(buf, sizeof(buf), "Manual:     %d / %d", manCount, NZONES); u8g2.drawStr(2, y, buf); y += 18;
  snprintf(buf, sizeof(buf), "Baterie:    %d %s", (int)voltage, voltage < LOW_BATTERY ? "SCAZUTA" : "OK");
  u8g2.drawStr(2, y, buf);
}

// Trimitere RAPIDA (~1s): prag, timp si stare zone
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

// Trimitere DATE: valorile de senzor (apelata cand se schimba, din updateSimulation)
void sendData() {
  for (int i = 0; i < NZONES; i++) {
    Serial.print(i + 1);            Serial.print(' ');
    Serial.print(soil[i], 1);       Serial.print(' ');
    Serial.print(soilTemp[i], 1);   Serial.print(' ');
    Serial.print(airTemp, 1);       Serial.print(' ');
    Serial.print(airHum, 1);        Serial.print(' ');
    Serial.print(light, 1);         Serial.print(' ');
    Serial.print(pressure, 1);      Serial.print(' ');
    Serial.print((int)voltage);     Serial.print(' ');
    Serial.println((int)co2);
  }
}

// Comenzi de la aplicatia web 
void readSerialCommands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inLine.length() > 0) { handleCommand(inLine); inLine = ""; }
    } else {
      inLine += c;
      if (inLine.length() > 20) inLine = "";
    }
  }
}

void handleCommand(String s) {
  s.trim();
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
