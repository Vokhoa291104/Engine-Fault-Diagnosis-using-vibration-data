/*
 * VibSense Gateway — Feature Edition
 * ====================================
 * - Nhận 12 đặc trưng từ Node qua ESP-NOW
 * - Gửi POST JSON lên VibSense Cloud Flask server
 *
 * Payload JSON gửi lên:
 *   {
 *     "node": 1,
 *     "x_rms":0.012, "y_rms":0.011, "z_rms":0.034,
 *     "x_peak":0.05, "y_peak":0.04, "z_peak":0.09,
 *     "x_cf":4.1,    "y_cf":3.8,    "z_cf":2.6,
 *     "vrms":0.021,  "kurtosis":3.2, "skewness":0.1,
 *     "timestamp": "2025-05-14T10:00:00"
 *   }
 */

#include <WiFi.h>
#include <esp_now.h>
#include <HTTPClient.h>
#include <time.h>
#include "esp_wifi.h"

// ============================================================
//  ⚙️  CẤU HÌNH — CHỈ SỬA PHẦN NÀY
// ============================================================
const char* ssid      = "iPhone của Khoa";
const char* password  = "29112004";
const char* serverURL = "http://172.20.10.4:8001/sensor_data2/";

const int nodeId       = 1;
const int WIFI_TIMEOUT = 20;
const int HTTP_RETRIES = 3;
const int HTTP_TIMEOUT = 8000;
// ============================================================

// ── Struct nhận từ Node — phải khớp hoàn toàn ───────────────
typedef struct struct_features {
  float x_rms,   y_rms,   z_rms;
  float x_peak,  y_peak,  z_peak;
  float x_cf,    y_cf,    z_cf;
  float vrms;
  float kurtosis;
  float skewness;
  unsigned long timestamp;
} struct_features;

struct_features receivedData;
struct_features bufferData;
volatile bool   dataReady = false;

// ── NTP ──────────────────────────────────────────────────────
void initTime() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Đồng bộ NTP");
  int retry = 0;
  while (time(nullptr) < 1000000000UL && retry < 20) {
    delay(500); Serial.print("."); retry++;
  }
  Serial.println(time(nullptr) >= 1000000000UL
    ? "\n Đồng bộ thời gian OK!"
    : "\n Dùng millis() thay thế.");
}

String getTimestamp() {
  time_t now = time(nullptr);
  if (now < 1000000000UL) return "T+" + String(millis() / 1000) + "s";
  struct tm* ti = localtime(&now);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", ti);
  return String(buf);
}

// ── Reconnect WiFi ───────────────────────────────────────────
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("WiFi mất kết nối, thử lại...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  int elapsed = 0;
  while (WiFi.status() != WL_CONNECTED && elapsed < WIFI_TIMEOUT * 2) {
    delay(500); Serial.print("."); elapsed++;
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) { Serial.println("\nKết nối lại OK! IP: " + WiFi.localIP().toString()); }
  else    { Serial.println("\nKhông thể kết nối lại WiFi."); }
  return ok;
}

// ── Gửi POST lên Cloud ───────────────────────────────────────
void sendToServer(struct_features& d) {
  if (!ensureWiFi()) return;

  String ts = getTimestamp();

  // Build JSON với 12 đặc trưng
  String payload = "{";
  payload += "\"node\":"      + String(nodeId)        + ",";
  payload += "\"x_rms\":"     + String(d.x_rms,   4)  + ",";
  payload += "\"y_rms\":"     + String(d.y_rms,   4)  + ",";
  payload += "\"z_rms\":"     + String(d.z_rms,   4)  + ",";
  payload += "\"x_peak\":"    + String(d.x_peak,  4)  + ",";
  payload += "\"y_peak\":"    + String(d.y_peak,  4)  + ",";
  payload += "\"z_peak\":"    + String(d.z_peak,  4)  + ",";
  payload += "\"x_cf\":"      + String(d.x_cf,    4)  + ",";
  payload += "\"y_cf\":"      + String(d.y_cf,    4)  + ",";
  payload += "\"z_cf\":"      + String(d.z_cf,    4)  + ",";
  payload += "\"vrms\":"      + String(d.vrms,    4)  + ",";
  payload += "\"kurtosis\":"  + String(d.kurtosis,4)  + ",";
  payload += "\"skewness\":"  + String(d.skewness,4)  + ",";
  payload += "\"timestamp\":\"" + ts + "\"";
  payload += "}";

  Serial.println("\nGửi: " + payload);

  bool success = false;
  for (int attempt = 1; attempt <= HTTP_RETRIES; attempt++) {
    HTTPClient http;
    http.begin(serverURL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT);

    int code = http.POST(payload);
    if (code == 200 || code == 201) {
      Serial.printf("OK (lần %d) | HTTP %d | %s\n",
                    attempt, code, http.getString().c_str());
      success = true;
      http.end();
      break;
    } else {
      Serial.printf("Thất bại lần %d | HTTP %d\n", attempt, code);
      if (attempt < HTTP_RETRIES) delay(1000);
    }
    http.end();
  }

  if (!success)
    Serial.println("Gửi thất bại sau " + String(HTTP_RETRIES) + " lần.");
}

// ── Callback ESP-NOW ─────────────────────────────────────────
void OnDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(receivedData)) {
    Serial.printf("Kích thước gói không khớp: nhận %d, cần %d\n",
                  len, sizeof(receivedData));
    return;
  }

  memcpy(&receivedData, data, sizeof(receivedData));

  Serial.printf(
    "\nNhận từ %02X:%02X:%02X:%02X:%02X:%02X\n"
    "  RMS  [%.4f %.4f %.4f]\n"
    "  Peak [%.4f %.4f %.4f]\n"
    "  CF   [%.2f  %.2f  %.2f]\n"
    "  Vrms=%.4f | Kurt=%.2f | Skew=%.2f\n",
    info->src_addr[0], info->src_addr[1], info->src_addr[2],
    info->src_addr[3], info->src_addr[4], info->src_addr[5],
    receivedData.x_rms,  receivedData.y_rms,  receivedData.z_rms,
    receivedData.x_peak, receivedData.y_peak, receivedData.z_peak,
    receivedData.x_cf,   receivedData.y_cf,   receivedData.z_cf,
    receivedData.vrms,   receivedData.kurtosis, receivedData.skewness
  );

  bufferData = receivedData;
  dataReady  = true;
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n====== VibSense Gateway (Feature Edition) ======");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Kết nối WiFi");

  int elapsed = 0;
  while (WiFi.status() != WL_CONNECTED && elapsed < WIFI_TIMEOUT * 2) {
    delay(500); Serial.print("."); elapsed++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nKết nối WiFi OK! IP: " + WiFi.localIP().toString());
    Serial.println("Kênh WiFi: " + String(WiFi.channel()));
    initTime();
    esp_wifi_set_channel(11, WIFI_SECOND_CHAN_NONE);
    Serial.println("ESP-NOW kênh: 11");
  } else {
    Serial.println("\nKhông kết nối WiFi! Sẽ thử lại khi gửi.");
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init thất bại! Restart...");
    ESP.restart();
  }

  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("Gateway sẵn sàng nhận dữ liệu 12 đặc trưng từ Node.");
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  if (dataReady) {
    dataReady = false;
    sendToServer(bufferData);
  }
  delay(10);
}
