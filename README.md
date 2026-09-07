# HydraSmart — Local Monitoring & Control Interface for a LoRa Irrigation System

A monitoring and control interface for an automated irrigation system built around
**Arduino MKR WAN 1310** boards communicating over **LoRa**. The project adds a
local control panel (custom PCB with OLED, buttons, switches, potentiometers and a
microSD reader) and a **Flask + web** application, extending an existing irrigation
system without modifying its architecture.

> Bachelor's thesis project — *"Monitoring and control interface for an irrigation
> system"*. The interface works fully offline (local server, no internet required).

## Overview

Water is a scarce resource, and traditional timer-based irrigation wastes it by
watering regardless of the actual soil state. Modern IoT solutions solve part of
this but often depend entirely on the internet and the cloud, becoming useless in
remote fields with poor connectivity. This project focuses on the part most such
systems neglect: a **local, autonomous interface** that lets the operator see the
system state and act directly in the field, without any external connection — while
still offering optional remote monitoring and control from a browser.

## Features

- **Soil-based automatic irrigation** across **6 independent zones**
- **Manual control** of each zone (On / Off / Auto) from the web interface
- **Real-time web dashboard**: zone status, environmental parameters, live charts
- **Local OLED interface** with 3 pages (Zones / Environment / Diagnostics)
- **Long-range wireless** sensor-to-controller link over **LoRa (868 MHz)**
- **Solar-powered sensor node** with deep-sleep operation and battery monitoring
- **Low-battery alert** on both the OLED and the web interface
- **Local data logging** to a microSD card
- **Simulation modes** (on the board and on the server) for demos without hardware
- **Robust by design**: irrigation keeps running even if the PC is disconnected

## Screenshots

> Place the image files in an `images/` folder in the repository root, then the
> links below will render on GitHub. Rename the files to match, or adjust the paths.

| Web dashboard | Local OLED interface |
|---|---|
| ![Web dashboard](images/web-dashboard.png) | ![OLED interface](images/oled-zones.png) |

| Sensor node (solar) | Full system watering a plant |
|---|---|
| ![Sensor node](images/sensor-node.png) | ![Full system](images/full-system.png) |

## System Architecture

```
Sensor node        ──LoRa──►   Actuator node       ──USB──►   Local server    ──HTTP──►   Web interface
(MKR WAN 1310)                 (MKR WAN 1310)                 (Python + Flask)            (browser)
solar + battery                irrigation logic               reads serial port          real-time UI
reads soil sensor              6 zones + relays               exposes a REST API          zones + charts
```

- **Data flow** (field → user): the sensor node measures and transmits over LoRa;
  the actuator node applies the irrigation logic and forwards data over USB; the
  server reads the serial port and exposes a REST API; the browser polls it and
  updates the display.
- **Command flow** (user → field): a button press in the browser is sent to the
  server, forwarded over USB to the actuator node, which drives the corresponding
  zone — closing the monitoring-and-control loop.

## Hardware

| Component | Role |
|-----------|------|
| Arduino MKR WAN 1310 (×2) | Sensor node and actuator node (32-bit, integrated LoRa) |
| Watermark sensor | Soil moisture, as matric potential in **kPa** (currently installed) |
| SH1107 OLED (128×128, I²C) | Local display |
| 2× potentiometers | Set the moisture threshold and the watering duration |
| Switches / push-buttons | Local commands and OLED navigation |
| microSD reader (SPI) | Local data logging |
| Relays + fuses | Drive the electrovalves — 8× 24 VAC and 2× 230 VAC outputs |
| Solar panel + battery | Energy-autonomous sensor node |

> The sensor node is **designed** for a full environmental sensor set (soil/air
> temperature, air humidity, light, pressure, CO₂). At this stage only the Watermark
> soil-moisture sensor is physically fitted; the other values are demonstrative until
> the sensors are installed. The irrigation decision uses the real soil measurement.

## Firmware

Three sketches share the same behavior and serial protocol:

- **`IrigatieController/`** — **unified controller** with a compile-time switch:
  - `#define SIMULATE 0` → real data received over LoRa
  - `#define SIMULATE 1` → data generated on-board (demo mode)
  - `#define USE_SD 1` → optional logging to microSD
  In simulation mode, generated data is fed through the **same** `handlePacket()`
  path as real data, so display and control logic are identical in both modes.
- **`IrigatieControllerLoRa/`** — controller receiving real LoRa data (6 zones).
- **`IrigatieControllerStandalone/`** — on-board simulation demo (no field nodes).
- **`LoRaSenderUSAMV_N1_v2/`** — sensor-node firmware (reads sensors, transmits over
  LoRa, logs to SD, enters deep sleep).

> **SAMD note:** on the MKR WAN 1310, `sscanf("%f", ...)` does not work; the LoRa
> payload is parsed manually with `toFloat()`.

## Serial Data Format (actuator node → PC)

The controller writes several line types that the server recognizes:

| Line (example) | Meaning |
|----------------|---------|
| `1 28.0 18.5 24.3 55.2 1200.0 1013.0 4128` | zone data: `ID soil soilTemp airTemp airHum light pressure voltage [co2]` |
| `analogValue1 = 770 => irrigation_treshold = 30` | moisture threshold (kPa) |
| `analogValue2 = 600 => irrigation_time = 90` | watering duration (s) |
| `ON1` / `OFF1` … `ON6` / `OFF6` | relay state of a zone |
| `ZMODE 3 auto` \| `manon` \| `manoff` | zone mode |

> **About "soil moisture":** the value comes from the **Watermark** sensor and
> represents soil matric potential in **kPa** — a higher value means drier soil.
> A zone starts watering when the value **reaches or exceeds the threshold**.

## Getting Started

Requires **Python 3.9+**.

```bash
cd server
python -m venv .venv
# Windows:
.\.venv\Scripts\Activate.ps1
# Linux/macOS:
# source .venv/bin/activate
pip install -r requirements.txt
python app.py
```

Then open <http://127.0.0.1:5000> in a browser.

### Simulation mode (no hardware)

If no serial port is found, the server starts in **simulation mode** automatically.
To force it:

```bash
python app.py --simulate
```

### Connecting the real board

The server auto-detects the port. To set it manually (and the baud rate matches the
sketch: **9600**):

```bash
# Windows PowerShell
$env:IRRIG_PORT = "COM3"; python app.py
```

Available ports are listed at <http://127.0.0.1:5000/api/ports>.

> **Note:** only one program can use the serial port at a time — close the Arduino
> IDE *Serial Monitor* before starting the server.

## REST API

| Method | Route | Description |
|--------|-------|-------------|
| GET | `/api/state` | Full state (thresholds, zones, modes, node data) |
| GET | `/api/history?node=1` | Measurement history for a zone (charting) |
| POST | `/api/command` | Manual zone control: `{"action":"zone","zone":3,"value":"on\|off\|auto"}` |
| GET | `/api/ports` | Available serial ports (debugging) |

## Project Structure

```
.
├── IrigatieController/               # unified controller (SIMULATE / USE_SD switches)
├── IrigatieControllerLoRa/           # controller — real LoRa data, 6 zones
├── IrigatieControllerStandalone/     # on-board simulation demo
├── LoRaSenderUSAMV_N1_v2/            # sensor-node firmware
├── ReceiverUSAMV_WIPv0.ino           # original receiver firmware
├── ReceiverUSAMV_WIPv0_fixed/        # receiver with the ID-comparison bug fixed
├── server/
│   ├── app.py                        # Flask server + REST API
│   ├── irrigation.py                 # serial reading/parsing + simulation mode
│   └── requirements.txt
├── web/
│   ├── index.html                    # dashboard
│   ├── style.css
│   └── app.js
├── PCB_Project1.pdf                  # PCB layout
├── Schema electrica.pdf              # schematic
└── README.md
```

## Notes on the Evolution of the Code

The project started from an existing prototype (on perfboard) whose receiver
firmware had several issues later fixed during development, e.g.:

```c
if (ID == 1) { ... }   // correct
if (ID = 2)  { ... }   // BUG: assignment instead of comparison -> always true
```

`if (ID = 2)` **assigns** 2 to `ID`, so the condition is always true and every
packet triggered the wrong zones. The final controller uses proper per-zone
handling, manual `toFloat()` parsing, correct watering timing, 6 zones and remote
control from the web.

## Possible Extensions

- Fit the remaining environmental sensors on the sensor node
- Support multiple sensor nodes (one per zone) for larger areas
- Replace the local PC with a standalone device / internet access
- Long-term data storage and more advanced decision algorithms (e.g. weather forecast)
- Secure the communication between nodes and the web access

## License

Academic project. Feel free to reference or adapt it; please credit the author.
