"""
Citirea si interpretarea datelor venite de la placa RECEIVER prin USB (serial).

Arhitectura reala a sistemului:

    Nod(uri) senzor  ──LoRa──►  Receiver (USAMV)  ──USB/Serial──►  Acest server  ──►  Browser
    (LoRaSender...)              (ReceiverUSAMV)                    (Flask)            (interfata web)

Receiver-ul este placa conectata la laptop. Pe portul serial el scrie mai multe
tipuri de linii, pe care acest modul le recunoaste:

  1) Linia de DATE retransmisa de la un nod (payload-ul LoRa brut), 8 valori:
        ID  umidSol  tempSol  tempAer  umidAer  lumina  presiune  tensiune
     ex:  1 28.0 18.5 24.3 55.2 1200.0 1013.0 4128

  2) Pragul de irigare (din potentiometru):
        analogValue1 = 770 => irrigation_treshold = 30
  3) Durata de udare (din potentiometru):
        analogValue2 = 600 => irrigation_time = 90
  4) Starea unei zone (releu) cand soseste un pachet:
        ON1 / OFF1 ... ON4 / OFF4
  5) Starea butoanelor:
        Button1: 1 | Button2: 1 | Button3: 0 | Button4: 1

Daca portul serial nu e disponibil, modulul porneste in MOD SIMULARE si
genereaza exact aceleasi tipuri de linii, ca sa se poata dezvolta/demonstra
interfata fara hardware.
"""

import math
import random
import re
import threading
import time
from collections import deque
from datetime import datetime

try:
    import serial           
    import serial.tools.list_ports
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False

RE_THRESHOLD = re.compile(r"irrigation_treshold\s*=\s*(\d+)")
RE_TIME = re.compile(r"irrigation_time\s*=\s*(\d+)")
RE_ZONE = re.compile(r"^(ON|OFF)(10|[1-9])$")
RE_ZMODE = re.compile(r"^ZMODE\s+(10|[1-9])\s+(auto|manon|manoff)$", re.IGNORECASE)
RE_BUTTONS = re.compile(
    r"Button1:\s*(\d).*Button2:\s*(\d).*Button3:\s*(\d).*Button4:\s*(\d)"
)

DATA_FIELDS = ["soil", "soil_temp", "air_temp", "air_hum", "light", "pressure", "voltage", "co2"]

class IrrigationManager:
    def __init__(self, port=None, baudrate=9600, history_size=300, simulate=False):
        self.port = port
        self.baudrate = baudrate
        self.simulate = simulate or not SERIAL_AVAILABLE

        self.state = {
            "connected": False,
            "simulated": self.simulate,
            "last_update": None,
            "threshold": None,          
            "irrigation_time": None,    
            "zones": {str(i): {"relay": 0, "mode": "auto"} for i in range(1, 7)},
            "buttons": {},              
            "nodes": {},               
        }

        self._sim_modes = {i: "auto" for i in range(1, 7)}

        self.history = {}               
        self._history_size = history_size

        self._serial = None
        self._lock = threading.Lock()
        self._running = False
        self._thread = None
        self._start_time = time.time()  

    def start(self):
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._serial:
            try:
                self._serial.close()
            except Exception:
                pass

    def get_state(self):
        with self._lock:
            s = dict(self.state)
            s["zones"] = {k: dict(v) for k, v in self.state["zones"].items()}
            s["buttons"] = dict(self.state["buttons"])
            s["nodes"] = {k: dict(v) for k, v in self.state["nodes"].items()}
            s["uptime"] = int(time.time() - self._start_time) 
            return s

    def get_history(self, node_id=None):
        with self._lock:
            if node_id is not None:
                return list(self.history.get(str(node_id), []))
            return {nid: list(h) for nid, h in self.history.items()}

    @staticmethod
    def list_ports():
        if not SERIAL_AVAILABLE:
            return []
        return [
            {"device": p.device, "description": p.description}
            for p in serial.tools.list_ports.comports()
        ]

    def send_zone_command(self, zone, value):
        """
        Trimite catre placa o comanda de control manual pe o zona.
        zone: 1..10 ; value: 'on' | 'off' | 'auto'
        """
        try:
            zone = int(zone)
        except (TypeError, ValueError):
            return False
        val = str(value).lower()
        cmd_map = {"on": "ON", "off": "OFF", "auto": "AUTO"}
        v = cmd_map.get(val)
        if v is None or not (1 <= zone <= 10):
            return False

        with self._lock:
            z = str(zone)
            if self.simulate:
                self._sim_modes[zone] = val
            if val == "on":
                self.state["zones"][z]["mode"] = "manon"
                self.state["zones"][z]["relay"] = 1
            elif val == "off":
                self.state["zones"][z]["mode"] = "manoff"
                self.state["zones"][z]["relay"] = 0
            else:  
                self.state["zones"][z]["mode"] = "auto"

        if self.simulate:
            return True

        line = f"Z{zone} {v}\n"
        if self._serial and self._serial.is_open:
            try:
                self._serial.write(line.encode("utf-8"))
                return True
            except Exception:
                return False
        return False

    def _run(self):
        if self.simulate:
            self._run_simulation()
        else:
            self._run_serial()

    def _run_serial(self):
        while self._running:
            if self._serial is None or not self._serial.is_open:
                if not self._try_open():
                    self._set_connected(False)
                    time.sleep(2)
                    continue
            try:
                raw = self._serial.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="ignore").strip()
                if line:
                    self._handle_line(line)
            except Exception:
                self._set_connected(False)
                try:
                    self._serial.close()
                except Exception:
                    pass
                self._serial = None
                time.sleep(2)

    def _try_open(self):
        port = self.port or self._autodetect_port()
        if not port:
            return False
        try:
            self._serial = serial.Serial(port, self.baudrate, timeout=2)
            time.sleep(2)
            self.port = port
            self._set_connected(True)
            return True
        except Exception:
            self._serial = None
            return False

    def _autodetect_port(self):
        for p in serial.tools.list_ports.comports():
            desc = (p.description or "").lower()
            if any(k in desc for k in ("arduino", "mkr", "usb serial", "wch", "ch340")):
                return p.device
        ports = list(serial.tools.list_ports.comports())
        return ports[0].device if ports else None

    def _handle_line(self, line):
        line = line.strip()
        if not line:
            return

        if self._try_parse_data(line):
            self._touch()
            return

        m = RE_THRESHOLD.search(line)
        if m:
            with self._lock:
                self.state["threshold"] = int(m.group(1))
            self._touch()
            return

        m = RE_TIME.search(line)
        if m:
            with self._lock:
                self.state["irrigation_time"] = int(m.group(1))
            self._touch()
            return

        m = RE_ZONE.match(line)
        if m:
            on = m.group(1) == "ON"
            zone = m.group(2)
            with self._lock:
                self.state["zones"][zone]["relay"] = 1 if on else 0
            self._touch()
            return

        m = RE_ZMODE.match(line)
        if m:
            zone = m.group(1)
            mode = m.group(2).lower()
            with self._lock:
                self.state["zones"][zone]["mode"] = mode
            self._touch()
            return

        m = RE_BUTTONS.search(line)
        if m:
            with self._lock:
                for i in range(4):
                    self.state["buttons"][str(i + 1)] = int(m.group(i + 1))
            self._touch()
            return

    def _try_parse_data(self, line):
        tokens = line.split()
        if len(tokens) not in (8, 9):  
            return False
        try:
            values = [float(t) for t in tokens]
        except ValueError:
            return False

        node_id = str(int(values[0]))
        now = datetime.now().isoformat(timespec="seconds")

        record = {}
        for i, field in enumerate(DATA_FIELDS):
            if i + 1 < len(values):
                record[field] = round(values[i + 1], 1)
        record["last_update"] = now

        with self._lock:
            self.state["nodes"][node_id] = record
            if node_id not in self.history:
                self.history[node_id] = deque(maxlen=self._history_size)
            self.history[node_id].append({
                "time": now,
                "soil": record["soil"],
                "soil_temp": record["soil_temp"],
                "air_temp": record["air_temp"],
                "air_hum": record["air_hum"],
            })
        return True

    def _touch(self):
        with self._lock:
            self.state["connected"] = True
            self.state["last_update"] = datetime.now().isoformat(timespec="seconds")

    def _set_connected(self, value):
        with self._lock:
            self.state["connected"] = value

    def _run_simulation(self):
        """
        Genereaza linii identice ca format cu cele emise de sketch-ul standalone
        si le trece prin acelasi parser (_handle_line), pentru a imita fidel
        sistemul real. Simuleaza 10 zone (cate un punct de masura pe zona).
        """
        with self._lock:
            self.state["simulated"] = True

        n = 6
        threshold = 30         
        irrigation_time = 90   

        soil = [20.0 + (i * 5) % 30 for i in range(n)]
        soil_temp = [18.0 + (i % 3) * 0.5 for i in range(n)]
        relays = [0] * n
        irrigating = [0] * n
        irr_start = [0.0] * n
        air_temp = air_hum = light = pressure = co2 = 0.0
        voltage = 4100.0          
        t0 = time.time()
        last_val = -1000.0
        VALUE_INTERVAL = 10.0  

        while self._running:
            elapsed = time.time() - t0

            if elapsed - last_val >= VALUE_INTERVAL:
                last_val = elapsed
                light = max(0, 1500 * (0.5 + 0.5 * math.sin(elapsed / 120)))
                # Mai multa lumina -> aer mai cald (corelatie lumina <-> temperatura)
                air_temp = 19 + (light / 1500.0) * 8 + random.uniform(-0.3, 0.3)
                air_hum = 55 + 8 * math.sin(elapsed / 90 + 1) + random.uniform(-1, 1)
                pressure = 1013 + random.uniform(-1.5, 1.5)
                co2 = max(350, 600 + 250 * math.sin(elapsed / 80) + random.uniform(-20, 20))
                voltage -= random.uniform(8, 16)
                if voltage < 3820:
                    voltage = 4150.0
                voltage = max(3700.0, min(4150.0, voltage))
                for i in range(n):
                    if relays[i]:
                        soil[i] = max(5.0, soil[i] - random.uniform(2, 4))
                    else:
                        soil[i] = min(80.0, soil[i] + random.uniform(0.4, 1.2))
                    soil_temp[i] = 16.0 + (soil[i] - 5.0) / 75.0 * 10.0 + random.uniform(-0.5, 0.5)
                    self._handle_line(
                        f"{i+1} {soil[i]:.1f} {soil_temp[i]:.1f} {air_temp:.1f} "
                        f"{air_hum:.1f} {light:.1f} {pressure:.1f} {voltage:.0f} {co2:.0f}"
                    )

            for i in range(n):
                with self._lock:
                    mode = self._sim_modes.get(i + 1, "auto")

                if mode == "on":
                    irrigating[i] = 0; relays[i] = 1
                elif mode == "off":
                    irrigating[i] = 0; relays[i] = 0
                else:                     
                    if soil[i] < threshold:
                        relays[i] = 0; irrigating[i] = 0
                    else:
                        if not irrigating[i]:
                            irrigating[i] = 1; irr_start[i] = elapsed
                        if elapsed - irr_start[i] >= irrigation_time:
                            relays[i] = 0; irrigating[i] = 0   
                        else:
                            relays[i] = 1

                with self._lock:
                    z = str(i + 1)
                    self.state["zones"][z]["relay"] = relays[i]
                    self.state["zones"][z]["mode"] = {"on": "manon", "off": "manoff"}.get(mode, "auto")

            self._handle_line(f"analogValue1 = 770 => irrigation_treshold = {threshold}")
            self._handle_line(f"analogValue2 = 600 => irrigation_time = {irrigation_time}")

            time.sleep(1)  