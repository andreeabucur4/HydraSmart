"""
Server local pentru aplicatia de monitorizare a sistemului de irigatii.

Rol:
  - citeste datele scoase pe USB de placa RECEIVER (modulul irrigation.py);
  - expune un API REST consumat de interfata web;
  - serveste fisierele interfetei (HTML/CSS/JS).

Pornire:
    python app.py
apoi deschide in browser: http://127.0.0.1:5000

Optiuni prin variabile de mediu:
    IRRIG_PORT      -> portul serial al receiver-ului (ex. COM3 / /dev/ttyACM0)
    IRRIG_BAUD      -> viteza serial (implicit 9600, ca in sketch)
    IRRIG_SIMULATE  -> "1" pentru a forta modul simulare (fara hardware)
"""

import os
import sys

from flask import Flask, jsonify, request, send_from_directory

from irrigation import IrrigationManager

WEB_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "web"))

app = Flask(__name__, static_folder=None)

PORT = os.environ.get("IRRIG_PORT")            
BAUD = int(os.environ.get("IRRIG_BAUD", "9600"))
SIMULATE = os.environ.get("IRRIG_SIMULATE", "0") == "1" or "--simulate" in sys.argv

if not SIMULATE and PORT is None and not IrrigationManager.list_ports():
    print("(!) Niciun port serial gasit -> pornesc in MOD SIMULARE.")
    SIMULATE = True

manager = IrrigationManager(port=PORT, baudrate=BAUD, simulate=SIMULATE)
manager.start()

@app.route("/")
def index():
    return send_from_directory(WEB_DIR, "index.html")

@app.route("/<path:filename>")
def static_files(filename):
    return send_from_directory(WEB_DIR, filename)

@app.route("/api/state")
def api_state():
    """Starea completa: praguri, zone (relee), butoane si datele fiecarui nod."""
    return jsonify(manager.get_state())

@app.route("/api/history")
def api_history():
    """
    Istoricul masuratorilor pentru grafice.
    Optional: /api/history?node=1 pentru un singur nod.
    """
    node = request.args.get("node")
    return jsonify(manager.get_history(node))

@app.route("/api/command", methods=["POST"])
def api_command():
    """
    Comanda de control manual pe o zona, trimisa catre placa.
    Corp JSON: {"action": "zone", "zone": 3, "value": "on" | "off" | "auto"}
    """
    data = request.get_json(silent=True) or {}
    if data.get("action") == "zone":
        ok = manager.send_zone_command(data.get("zone"), data.get("value"))
        return jsonify({"ok": ok})
    return jsonify({"ok": False, "error": "comanda necunoscuta"}), 400

@app.route("/api/ports")
def api_ports():
    """Porturi seriale disponibile (utile pentru depanare)."""
    return jsonify(IrrigationManager.list_ports())


if __name__ == "__main__":
    print("=" * 55)
    print("  Server monitorizare irigatii pornit")
    print("  Interfata web:  http://127.0.0.1:5000")
    print("  Mod simulare:  ", "DA" if manager.simulate else "NU (serial)")
    print("=" * 55)
    port = int(os.environ.get("PORT", "5000"))  
    app.run(host="127.0.0.1", port=port, debug=False, use_reloader=False)