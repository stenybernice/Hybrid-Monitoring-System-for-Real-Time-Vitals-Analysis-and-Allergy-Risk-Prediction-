from flask import Flask, request, jsonify, send_from_directory
from flask_cors import CORS
import joblib
import numpy as np
import serial
import threading
import time

app = Flask(__name__)
CORS(app) # This allows the browser to communicate with the server

# =========================
# LOAD MODEL
# =========================
# Ensure this path is correct for your system
MODEL_PATH = "C:/Users/ensty/OneDrive/Documents/report/minipp/proj/allergy_model.pkl"
model = joblib.load(MODEL_PATH)

features = [
    "Phl_p_1", "Fel_d_1", "Der_p_2",
    "Der_f_2", "Der_p_1", "Cup_a_1", "Cyn_d_1"
]

# =========================
# SERIAL SETUP
# =========================
ser = None

def init_serial():
    global ser
    try:
        # Verify COM7 is the correct port for your ESP32/Arduino
        ser = serial.Serial('COM7', 115200, timeout=1)
        time.sleep(2)  
        print("Serial connected successfully on COM7")
    except Exception as e:
        print("Serial connection failed:", e)
        ser = None

latest_vitals = {
    "hr": 0,
    "spo2": 0,
    "temp": 0,
    "risk": 0,
    "status": "WAITING"
}

# =========================
# SERIAL THREAD
# =========================
def read_serial():
    global latest_vitals, ser

    while True:
        if ser is None:
            time.sleep(2)
            continue

        try:
            line = ser.readline().decode(errors="ignore").strip()
            if not line:
                continue

            # Assuming format: HR: 75 | SpO2: 98% | Temp: 36.5C
            if "HR:" in line:
                parts = line.split("|")
                latest_vitals["hr"] = float(parts[0].split(":")[1].strip().split()[0])
                latest_vitals["spo2"] = float(parts[1].split(":")[1].strip().replace("%", ""))
                latest_vitals["temp"] = float(parts[2].split(":")[1].strip().replace("C", ""))

            # Assuming format: Risk: | Val: 0.5 | Status: Stable
            elif "Risk:" in line:
                parts = line.split("|")
                latest_vitals["risk"] = float(parts[1].split(":")[1].strip())
                latest_vitals["status"] = parts[2].strip()

        except Exception as e:
            # Silently handle parsing errors to keep thread alive
            continue

def start_thread():
    t = threading.Thread(target=read_serial, daemon=True)
    t.start()

# =========================
# ROUTES
# =========================
@app.route('/')
def home():
    # Serves index.html from the same folder
    return send_from_directory('.', 'index.html')

@app.route('/predict', methods=['POST'])
def predict():
    try:
        data = request.json
        input_data = np.array([[data[f] for f in features]])

        prediction = model.predict(input_data)[0]
        prob = model.predict_proba(input_data)[0][1]

        return jsonify({
            "prediction": "Allergy Detected" if prediction == 1 else "No Allergy",
            "risk_score": round(float(prob), 3)
        })
    except Exception as e:
        return jsonify({"error": str(e)}), 400

@app.route('/vitals', methods=['GET'])
def vitals():
    return jsonify(latest_vitals)

# =========================
# RUN
# =========================
if __name__ == "__main__":
    init_serial()
    start_thread()
    # use_reloader=False is important when using Serial threads
    app.run(host='0.0.0.0', port=5000, debug=True, use_reloader=False)