# VibSense Cloud — Hệ thống chẩn đoán lỗi động cơ

**ĐATN: Hệ thống giám sát rung động & chẩn đoán lỗi động cơ (Võ Anh Khoa – BKHN 2026)**

---

## Cấu trúc thư mục

```
vibration-cloud/
├── main.py                         # FastAPI backend (API + ML inference)
├── requirements.txt                # Python dependencies
├── motor_fault_random_forest.pkl   # ← ĐẶT FILE MODEL CỦA BẠN VÀO ĐÂY
├── vibration.db                    # SQLite DB (tự tạo khi chạy lần đầu)
├── static/
│   └── index.html                  # Dashboard web
└── README.md
```

---

## Cài đặt & Chạy

### 1. Cài Python dependencies

```bash
pip install -r requirements.txt
```

### 2. Đặt file model ML vào thư mục gốc

```
vibration-cloud/motor_fault_random_forest.pkl
```

> Nếu không có file .pkl, hệ thống tự động dùng **rule-based (ISO 10816)** để phân loại.

### 3. Chạy server

```bash
cd vibration-cloud
python main.py
```

Server khởi động tại: `http://0.0.0.0:8001`

---

## API Endpoints

| Method | URL | Mô tả |
|--------|-----|-------|
| `POST` | `/sensor_data2/` | Nhận dữ liệu từ gateway |
| `GET`  | `/api/data` | Lấy dữ liệu thô theo ngày |
| `GET`  | `/api/summary` | Tóm tắt thống kê |
| `GET`  | `/api/hourly` | Dữ liệu trung bình theo giờ |
| `GET`  | `/api/dates` | Các ngày có dữ liệu |
| `GET`  | `/api/nodes` | Danh sách node |
| `GET`  | `/health` | Kiểm tra trạng thái |
| `GET`  | `/` | Dashboard web |

### Ví dụ POST từ gateway:

```json
{
  "node": 3,
  "x": 0.123,
  "y": 0.456,
  "z": 0.789,
  "vrms": 1.532
}
```

### Response từ server:

```json
{
  "id": 42,
  "node": 3,
  "x": 0.123, "y": 0.456, "z": 0.789, "vrms": 1.532,
  "rms_mag": 0.9297,
  "timestamp": "2026-05-08T10:30:00",
  "predicted": "normal",
  "predicted_vi": "Bình thường",
  "confidence": 0.92,
  "color": "#22c55e",
  "icon": "✅",
  "model_used": "ML-RandomForest"
}
```

---

## ⚠️ Bug trong gateway_FFT_1.ino cần sửa

File gateway hiện có 2 lỗi:

### Lỗi 1: `struct_message` thiếu trường `fft`

```cpp
// ❌ HIỆN TẠI (lỗi biên dịch)
typedef struct struct_message {
  float x, y, z, vrms;
} struct_message;

// Nhưng sendToServerJSON() lại gọi incomingData.fft[i] → lỗi!
```

**Sửa**: Xóa vòng lặp FFT hoặc thêm trường fft vào struct:

```cpp
// ✅ Nếu không cần FFT: xóa đoạn này trong sendToServerJSON()
// String fftString = "";
// for (int i = 0; i < SAMPLES / 2; i++) { ... }
```

### Lỗi 2: JSON bị thiếu dấu `}`

```cpp
// ❌ HIỆN TẠI
String json = "{";
json += "\"node\":" + String(nodeId) + ",";
json += "\"vrms\":" + String(incomingData.vrms, 3) + ",";
// Thiếu dòng đóng JSON!

// ✅ THÊM VÀO:
json += "\"timestamp\":\"" + timestamp + "\"";
json += "}";
```

### Code gateway đã sửa hoàn chỉnh:

```cpp
void sendToServerJSON() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverURL);
    http.addHeader("Content-Type", "application/json");

    String timestamp = getISO8601Timestamp(time(nullptr));

    String json = "{";
    json += "\"node\":" + String(nodeId) + ",";
    json += "\"x\":" + String(incomingData.x, 3) + ",";
    json += "\"y\":" + String(incomingData.y, 3) + ",";
    json += "\"z\":" + String(incomingData.z, 3) + ",";
    json += "\"vrms\":" + String(incomingData.vrms, 3) + ",";
    json += "\"timestamp\":\"" + timestamp + "\"";
    json += "}";

    Serial.println("Gửi JSON: " + json);
    int code = http.POST(json);
    Serial.println("HTTP: " + String(code));
    http.end();
  }
}
```

---

## Mapping Features cho ML Model

Gateway chỉ gửi **4 giá trị** (x, y, z, vrms), nhưng model cần **12 features**.
Server tự động tính các features còn lại:

| Feature model cần | Nguồn |
|-------------------|-------|
| `rms_x_g`         | ← `x` từ gateway |
| `rms_y_g`         | ← `y` từ gateway |
| `rms_z_g`         | ← `z` từ gateway |
| `rms_mag_g`       | Tính: `√(x²+y²+z²)` |
| `vrms_mm_s`       | ← `vrms` từ gateway |
| `peak_to_peak_g`  | Ước lượng: `rms_mag × 2.83` |
| `crest_factor`    | Mặc định: `1.414` (Gaussian) |
| `kurtosis`        | Ước lượng từ vrms |
| `dominant_freq_hz`| Mặc định: `50 Hz` |
| `band_1/2x_energy`| Mặc định tỉ lệ |
| `band_high_energy`| Mặc định tỉ lệ |

> **Để nâng độ chính xác**: Nâng cấp node gửi thêm dữ liệu FFT raw
> để server tính đúng các features tần số.

---

## Triển khai lên Cloud

### Option A: VPS/Server riêng (đang dùng 116.118.44.82)

```bash
# Cài screen để chạy nền
sudo apt install screen -y
screen -S vibcloud
python main.py
# Ctrl+A+D để detach
```

### Option B: Systemd service (production)

```ini
# /etc/systemd/system/vibcloud.service
[Unit]
Description=VibSense Cloud
After=network.target

[Service]
WorkingDirectory=/path/to/vibration-cloud
ExecStart=/usr/bin/python3 main.py
Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable vibcloud
sudo systemctl start vibcloud
```

---

## Nhãn lỗi (fault_label)

Đảm bảo nhãn trong training data của bạn trùng với các key sau:

| Key | Tên tiếng Việt |
|-----|----------------|
| `normal` | Bình thường |
| `unbalance` | Mất cân bằng |
| `misalignment` | Lệch trục |
| `bearing_fault` | Lỗi ổ lăn |
| `looseness` | Lỏng cơ học |

Nếu nhãn khác, chỉnh `FAULT_LABELS` trong `main.py`.
