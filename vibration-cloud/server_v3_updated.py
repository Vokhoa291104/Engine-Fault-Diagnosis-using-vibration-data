"""
VibSense Cloud Server v3
========================
Flask backend nhận 12 đặc trưng từ Gateway (ESP32),
lưu vào SQLite, chạy AI dự đoán lỗi động cơ.

Model features (12):
    x_rms, y_rms, z_rms         — RMS từng trục (g)
    x_peak, y_peak, z_peak      — Giá trị đỉnh (g)
    x_cf, y_cf, z_cf            — Crest Factor
    vrms                         — RMS tổng hợp 3 trục
    kurtosis                     — Độ nhọn xung
    skewness                     — Độ lệch phân phối

3–5 trạng thái: normal, looseness, bearing_fault, (unbalance, misalignment)

Yêu cầu:
    pip install flask flask-cors scikit-learn numpy pandas

Chạy:
    python server_v3_updated.py
"""

import os, math, pickle, sqlite3
from datetime import datetime
from flask import Flask, request, jsonify, send_from_directory, Response
from flask_cors import CORS
import numpy as np

# ── Cấu hình ──────────────────────────────────────────────
DB_FILE    = "vibsense.db"
MODEL_FILE = "motor_fault_random_forest.pkl"
PORT       = 8001
# ──────────────────────────────────────────────────────────

app = Flask(__name__, static_folder="static")
CORS(app)

# ── Label map ─────────────────────────────────────────────
LABEL_MAP = {
    "normal":        {"vi": "Bình thường",      "color": "#22c55e", "icon": "✅"},
    "unbalance":     {"vi": "Mất cân bằng",     "color": "#eab308", "icon": "🟡"},
    "misalignment":  {"vi": "Lệch trục",        "color": "#f97316", "icon": "🟠"},
    "looseness":     {"vi": "Lỏng lẻo cơ khí", "color": "#3b82f6", "icon": "🔵"},
    "bearing_fault": {"vi": "Hư vòng bi",       "color": "#ef4444", "icon": "🔴"},
}

# ── 12 cột đặc trưng ──────────────────────────────────────
FEATURE_COLS = [
    "x_rms", "y_rms", "z_rms",
    "x_peak", "y_peak", "z_peak",
    "x_cf", "y_cf", "z_cf",
    "vrms", "kurtosis", "skewness"
]

# ── Load AI model ──────────────────────────────────────────
model = None
model_classes = []

def load_model():
    global model, model_classes
    if os.path.exists(MODEL_FILE):
        try:
            with open(MODEL_FILE, "rb") as f:
                model = pickle.load(f)
            model_classes = list(model.classes_)
            print(f"✅ AI model loaded | Classes: {model_classes}")
            print(f"   Features: {FEATURE_COLS}")
        except Exception as e:
            print(f"⚠️  Không load được model: {e}")
    else:
        print(f"⚠️  Không tìm thấy {MODEL_FILE}")
        print(f"   → Chạy notebook để train và tạo model")

load_model()

# ── SQLite setup ───────────────────────────────────────────
def get_db():
    conn = sqlite3.connect(DB_FILE)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    with get_db() as db:
        # Xóa bảng cũ nếu có schema khác
        db.execute("DROP TABLE IF EXISTS sensor_data")
        db.execute("""
            CREATE TABLE IF NOT EXISTS sensor_data (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                node        INTEGER,
                x_rms       REAL,
                y_rms       REAL,
                z_rms       REAL,
                x_peak      REAL,
                y_peak      REAL,
                z_peak      REAL,
                x_cf        REAL,
                y_cf        REAL,
                z_cf        REAL,
                vrms        REAL,
                kurtosis    REAL,
                skewness    REAL,
                prediction  TEXT,
                confidence  REAL,
                timestamp   TEXT,
                created_at  TEXT DEFAULT (datetime('now','localtime'))
            )
        """)
        db.commit()
    print("✅ Database sẵn sàng:", DB_FILE)

init_db()

# ── AI Prediction ──────────────────────────────────────────
def run_prediction(features: dict):
    """
    Input:  dict chứa 12 đặc trưng
    Output: (predicted_class, confidence, all_probabilities_dict)
    """
    if model is None:
        return None, None, None

    vec   = np.array([[features[c] for c in FEATURE_COLS]])
    pred  = model.predict(vec)[0]
    proba = model.predict_proba(vec)[0]
    conf  = float(max(proba))
    all_p = {cls: float(p) for cls, p in zip(model_classes, proba)}
    return str(pred), conf, all_p

# ── API: nhận dữ liệu từ Gateway ──────────────────────────
@app.route("/sensor_data2/", methods=["POST"])
def receive_data():
    try:
        data = request.get_json(force=True)

        node      = int(data.get("node", 0))
        timestamp = str(data.get("timestamp", datetime.now().isoformat()))

        # Đọc 12 đặc trưng
        features = {c: float(data.get(c, 0)) for c in FEATURE_COLS}

        # Dự đoán AI
        pred, conf, all_proba = run_prediction(features)

        # Lưu vào DB
        with get_db() as db:
            db.execute("""
                INSERT INTO sensor_data
                    (node, x_rms, y_rms, z_rms,
                     x_peak, y_peak, z_peak,
                     x_cf, y_cf, z_cf,
                     vrms, kurtosis, skewness,
                     prediction, confidence, timestamp)
                VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """, (
                node,
                features["x_rms"],  features["y_rms"],  features["z_rms"],
                features["x_peak"], features["y_peak"], features["z_peak"],
                features["x_cf"],   features["y_cf"],   features["z_cf"],
                features["vrms"],   features["kurtosis"], features["skewness"],
                pred, conf, timestamp
            ))
            db.commit()

        # Log ra console
        label_info = LABEL_MAP.get(pred, {}) if pred else {}
        if pred:
            print(f"📥 Node{node} | vrms={features['vrms']:.3f} "
                  f"kurt={features['kurtosis']:.2f} skew={features['skewness']:.2f} "
                  f"| {label_info.get('icon','')} {pred} ({conf*100:.1f}%)")
        else:
            print(f"📥 Node{node} | vrms={features['vrms']:.3f} | (no model)")

        return jsonify({
            "status":            "ok",
            "prediction":        pred,
            "confidence":        conf,
            "label_vi":          label_info.get("vi"),
            "all_probabilities": all_proba,
        }), 200

    except Exception as e:
        print(f"❌ Lỗi nhận dữ liệu: {e}")
        return jsonify({"status": "error", "message": str(e)}), 400

# ── API: lấy dữ liệu gần đây ──────────────────────────────
@app.route("/api/recent", methods=["GET"])
def get_recent():
    node  = request.args.get("node",  type=int)
    limit = request.args.get("limit", 100, type=int)
    with get_db() as db:
        if node:
            rows = db.execute(
                "SELECT * FROM sensor_data WHERE node=? ORDER BY id DESC LIMIT ?",
                (node, limit)).fetchall()
        else:
            rows = db.execute(
                "SELECT * FROM sensor_data ORDER BY id DESC LIMIT ?",
                (limit,)).fetchall()
    return jsonify([dict(r) for r in rows])

# ── API: thống kê tổng quan ───────────────────────────────
@app.route("/api/stats", methods=["GET"])
def get_stats():
    with get_db() as db:
        total  = db.execute("SELECT COUNT(*) FROM sensor_data").fetchone()[0]
        nodes  = db.execute("SELECT DISTINCT node FROM sensor_data").fetchall()
        latest = db.execute(
            "SELECT * FROM sensor_data ORDER BY id DESC LIMIT 1").fetchone()
        pred_counts = db.execute(
            "SELECT prediction, COUNT(*) as cnt FROM sensor_data "
            "WHERE prediction IS NOT NULL GROUP BY prediction").fetchall()
    return jsonify({
        "total_records":   total,
        "nodes":           [r[0] for r in nodes],
        "latest":          dict(latest) if latest else None,
        "prediction_dist": {r["prediction"]: r["cnt"] for r in pred_counts},
    })

# ── API: dữ liệu cho chart (vrms là chính) ───────────────
@app.route("/api/chart", methods=["GET"])
def get_chart():
    node  = request.args.get("node",  type=int)
    limit = request.args.get("limit", 60, type=int)
    axis  = request.args.get("axis",  "vrms")

    # Chỉ cho phép các cột hợp lệ
    valid_axes = {"vrms", "x_rms", "y_rms", "z_rms",
                  "kurtosis", "skewness", "x_peak", "y_peak", "z_peak"}
    if axis not in valid_axes:
        axis = "vrms"

    with get_db() as db:
        q = (f"SELECT timestamp, {axis}, prediction FROM sensor_data "
             + ("WHERE node=? " if node else "")
             + "ORDER BY id DESC LIMIT ?")
        params = (node, limit) if node else (limit,)
        rows   = db.execute(q, params).fetchall()

    rows = list(reversed(rows))
    return jsonify({
        "labels":      [r["timestamp"]  for r in rows],
        "values":      [r[axis]         for r in rows],
        "predictions": [r["prediction"] for r in rows],
    })

# ── API: thông tin model ──────────────────────────────────
@app.route("/api/model_info", methods=["GET"])
def model_info():
    if model is None:
        return jsonify({"loaded": False,
                        "message": "Chưa có model. Chạy notebook để train."})
    return jsonify({
        "loaded":       True,
        "type":         type(model).__name__,
        "classes":      model_classes,
        "n_estimators": getattr(model, "n_estimators", None),
        "features":     FEATURE_COLS,
        "label_map":    LABEL_MAP,
    })

# ── API: reload model không cần restart server ────────────
@app.route("/api/reload_model", methods=["POST"])
def reload_model_api():
    load_model()
    if model:
        return jsonify({"status": "ok", "classes": model_classes})
    return jsonify({"status": "error", "message": "Không load được model"}), 500

# ── API: xuất CSV để train model ─────────────────────────
@app.route("/api/export_csv", methods=["GET"])
def export_csv():
    node = request.args.get("node", type=int)
    with get_db() as db:
        if node:
            rows = db.execute(
                "SELECT timestamp, x_rms, y_rms, z_rms, "
                "x_peak, y_peak, z_peak, x_cf, y_cf, z_cf, "
                "vrms, kurtosis, skewness "
                "FROM sensor_data WHERE node=? ORDER BY id ASC", (node,)
            ).fetchall()
        else:
            rows = db.execute(
                "SELECT timestamp, x_rms, y_rms, z_rms, "
                "x_peak, y_peak, z_peak, x_cf, y_cf, z_cf, "
                "vrms, kurtosis, skewness "
                "FROM sensor_data ORDER BY id ASC"
            ).fetchall()

    now      = datetime.now().strftime("%Y%m%d_%H%M%S")
    suffix   = f"_node{node}" if node else ""
    filename = f"vibsense{suffix}_{now}.csv"

    def generate():
        yield "timestamp,x_rms,y_rms,z_rms,x_peak,y_peak,z_peak," \
              "x_cf,y_cf,z_cf,vrms,kurtosis,skewness\n"
        for r in rows:
            yield (f"{r['timestamp']},"
                   f"{r['x_rms']},{r['y_rms']},{r['z_rms']},"
                   f"{r['x_peak']},{r['y_peak']},{r['z_peak']},"
                   f"{r['x_cf']},{r['y_cf']},{r['z_cf']},"
                   f"{r['vrms']},{r['kurtosis']},{r['skewness']}\n")

    print(f"📤 Xuất CSV: {len(rows)} bản ghi → {filename}")
    return Response(
        generate(),
        mimetype="text/csv",
        headers={"Content-Disposition": f"attachment; filename={filename}"}
    )

# ── Serve frontend ────────────────────────────────────────
@app.route("/")
def index():
    return send_from_directory("static", "index.html")

# ─────────────────────────────────────────────────────────
if __name__ == "__main__":
    print(f"\n{'='*52}")
    print(f"  VibSense Cloud v3  →  http://localhost:{PORT}")
    print(f"  API endpoint:  POST /sensor_data2/")
    print(f"  Export CSV:    GET  /api/export_csv")
    print(f"  Dashboard:     GET  /")
    print(f"{'='*52}\n")
    app.run(host="0.0.0.0", port=PORT, debug=False)
