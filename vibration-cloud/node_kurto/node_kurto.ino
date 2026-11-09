/*
 * VibSense Node — Feature Extraction Edition
 * ===========================================
 * ODR: 800Hz | Cửa sổ: 256 mẫu (~320ms)
 * Tính 12 đặc trưng trên node rồi gửi 1 gói qua ESP-NOW
 *
 * Đặc trưng gửi đi:
 *   x_rms, y_rms, z_rms        — biên độ RMS từng trục (g)
 *   x_peak, y_peak, z_peak     — giá trị đỉnh từng trục (g)
 *   x_cf, y_cf, z_cf           — crest factor từng trục
 *   vrms                        — RMS tổng hợp 3 trục (g)
 *   kurtosis                    — độ nhọn (nhạy với xung vòng bi)
 *   skewness                    — độ lệch phân phối
 */

#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>
#include "esp_wifi.h"
#include <math.h>

Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);

// ── Địa chỉ MAC Gateway ──────────────────────────────────────
uint8_t receiverAddress[] = {0xC8, 0x2E, 0x18, 0x8E, 0xF3, 0xE8};

// ── Struct gửi qua ESP-NOW — 12 đặc trưng ───────────────────
typedef struct struct_features {
  float x_rms,   y_rms,   z_rms;    // RMS từng trục (g)
  float x_peak,  y_peak,  z_peak;   // Giá trị đỉnh (g)
  float x_cf,    y_cf,    z_cf;     // Crest Factor
  float vrms;                        // RMS tổng hợp 3 trục
  float kurtosis;                    // Độ nhọn
  float skewness;                    // Độ lệch
  unsigned long timestamp;
} struct_features;

struct_features featData;

// ── Cấu hình thu thập ────────────────────────────────────────
#define WINDOW_SIZE   256    // Số mẫu mỗi cửa sổ
#define CALIB_SAMPLES 100    // Số mẫu tính offset lúc khởi động
#define SAMPLE_US     1250   // 1,000,000 / 800Hz = 1250µs/mẫu

float offsetX = 0, offsetY = 0, offsetZ = 0;

// Buffer thu thập
float bufX[WINDOW_SIZE];
float bufY[WINDOW_SIZE];
float bufZ[WINDOW_SIZE];

// ── Callback gửi ESP-NOW ─────────────────────────────────────
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("ESP-NOW: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Thành công" : "Thất bại");
}

// ── Hiệu chỉnh offset ────────────────────────────────────────
void calibrate() {
  Serial.println("Hiệu chỉnh cảm biến, giữ yên...");
  sensors_event_t event;

  // Bỏ qua 50 mẫu đầu để cảm biến ổn định
  for (int i = 0; i < 50; i++) {
    accel.getEvent(&event);
    delayMicroseconds(SAMPLE_US);
  }

  double sumX = 0, sumY = 0, sumZ = 0;
  for (int i = 0; i < CALIB_SAMPLES; i++) {
    accel.getEvent(&event);
    sumX += event.acceleration.x;
    sumY += event.acceleration.y;
    sumZ += event.acceleration.z;
    delayMicroseconds(SAMPLE_US);
  }

  offsetX = sumX / CALIB_SAMPLES;
  offsetY = sumY / CALIB_SAMPLES;
  offsetZ = sumZ / CALIB_SAMPLES;

  Serial.printf("Offset X=%.4f Y=%.4f Z=%.4f\n", offsetX, offsetY, offsetZ);
  Serial.println("Hiệu chỉnh xong!");
}

// ── Thu thập 256 mẫu vào buffer ──────────────────────────────
void collectWindow() {
  sensors_event_t event;
  unsigned long t0 = micros();

  for (int i = 0; i < WINDOW_SIZE; i++) {
    // Đợi đúng thời điểm lấy mẫu (non-blocking wait)
    while ((micros() - t0) < (unsigned long)i * SAMPLE_US);

    accel.getEvent(&event);
    bufX[i] = event.acceleration.x - offsetX;
    bufY[i] = event.acceleration.y - offsetY;
    bufZ[i] = event.acceleration.z - offsetZ;
  }
}

// ── Tính đặc trưng từ buffer ─────────────────────────────────
void computeFeatures() {
  double sumX2 = 0, sumY2 = 0, sumZ2 = 0;
  float  pkX = 0,   pkY = 0,   pkZ = 0;
  double meanX = 0, meanY = 0, meanZ = 0;

  // Pass 1: mean và peak
  for (int i = 0; i < WINDOW_SIZE; i++) {
    meanX += bufX[i];
    meanY += bufY[i];
    meanZ += bufZ[i];

    float ax = fabsf(bufX[i]);
    float ay = fabsf(bufY[i]);
    float az = fabsf(bufZ[i]);
    if (ax > pkX) pkX = ax;
    if (ay > pkY) pkY = ay;
    if (az > pkZ) pkZ = az;
  }
  meanX /= WINDOW_SIZE;
  meanY /= WINDOW_SIZE;
  meanZ /= WINDOW_SIZE;

  // Pass 2: RMS, kurtosis, skewness (dùng tín hiệu đã trừ mean)
  double m2 = 0, m3 = 0, m4 = 0;   // moment bậc 2, 3, 4 (dùng trục Z đại diện)

  for (int i = 0; i < WINDOW_SIZE; i++) {
    double dx = bufX[i] - meanX;
    double dy = bufY[i] - meanY;
    double dz = bufZ[i] - meanZ;

    sumX2 += dx * dx;
    sumY2 += dy * dy;
    sumZ2 += dz * dz;

    // Kurtosis & skewness tính trên vector tổng hợp
    double mag = sqrt(dx*dx + dy*dy + dz*dz);
    m2 += mag * mag;
    m3 += mag * mag * mag;
    m4 += mag * mag * mag * mag;
  }

  float rmsX = sqrt(sumX2 / WINDOW_SIZE);
  float rmsY = sqrt(sumY2 / WINDOW_SIZE);
  float rmsZ = sqrt(sumZ2 / WINDOW_SIZE);

  double mu2 = m2 / WINDOW_SIZE;
  double mu3 = m3 / WINDOW_SIZE;
  double mu4 = m4 / WINDOW_SIZE;

  // Gán vào struct
  featData.x_rms   = rmsX;
  featData.y_rms   = rmsY;
  featData.z_rms   = rmsZ;
  featData.x_peak  = pkX;
  featData.y_peak  = pkY;
  featData.z_peak  = pkZ;

  // Crest factor = Peak / RMS (tránh chia 0)
  featData.x_cf = (rmsX > 1e-6f) ? pkX / rmsX : 0.0f;
  featData.y_cf = (rmsY > 1e-6f) ? pkY / rmsY : 0.0f;
  featData.z_cf = (rmsZ > 1e-6f) ? pkZ / rmsZ : 0.0f;

  // Vrms tổng hợp
  featData.vrms = sqrt((rmsX*rmsX + rmsY*rmsY + rmsZ*rmsZ) / 3.0f);

  // Kurtosis = mu4 / mu2^2  (=3 với phân phối chuẩn, >3 có xung đột ngột)
  featData.kurtosis = (mu2 > 1e-12) ? (float)(mu4 / (mu2 * mu2)) : 0.0f;

  // Skewness = mu3 / mu2^(3/2)
  featData.skewness = (mu2 > 1e-12) ? (float)(mu3 / pow(mu2, 1.5)) : 0.0f;

  featData.timestamp = millis();
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Cấp nguồn cảm biến bằng GPIO
  pinMode(5, OUTPUT);
  pinMode(6, OUTPUT);
  digitalWrite(5, LOW);    // GND
  digitalWrite(6, HIGH);   // 3.3V
  delay(500);

  // I2C 400kHz để đọc đủ nhanh ở 800Hz ODR
  Wire.begin(20, 21);
  Wire.setClock(400000);

  if (!accel.begin()) {
    Serial.println("Không tìm thấy ADXL345!");
    while (1);
  }

  // Range ±8g — đủ rộng cho rung động mạnh
  accel.setRange(ADXL345_RANGE_8_G);

  // DATA_FORMAT: FULL_RES + ±8g
  Wire.beginTransmission(0x53);
  Wire.write(0x31);
  Wire.write(0x0A);   // FULL_RES | ±8g
  Wire.endTransmission();

  // BW_RATE: ODR = 800Hz
  // 0x0D = 800Hz | 0x0C = 400Hz | 0x0B = 200Hz | 0x0A = 100Hz
  Wire.beginTransmission(0x53);
  Wire.write(0x2C);
  Wire.write(0x0D);   // ODR 800Hz
  Wire.endTransmission();

  Serial.println("ADXL345 — Range: ±8g | ODR: 800Hz | I2C: 400kHz");

  calibrate();

  // Khởi tạo ESP-NOW
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init thất bại!");
    return;
  }

  // Set channel SAU khi init ESP-NOW
  ESP_ERROR_CHECK(esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE));
  Serial.println("ESP-NOW kênh: 5");

  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverAddress, 6);
  peerInfo.channel = 6;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Thêm peer thất bại!");
    return;
  }

  Serial.println("Node sẵn sàng — 256 mẫu/gói @ 800Hz");
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  // 1. Thu thập 256 mẫu (~320ms)
  collectWindow();

  // 2. Tính 12 đặc trưng
  computeFeatures();

  // 3. In ra Serial để kiểm tra
  Serial.printf(
    "RMS[%.3f %.3f %.3f] Peak[%.3f %.3f %.3f] CF[%.2f %.2f %.2f] Vrms=%.3f Kurt=%.2f Skew=%.2f\n",
    featData.x_rms,  featData.y_rms,  featData.z_rms,
    featData.x_peak, featData.y_peak, featData.z_peak,
    featData.x_cf,   featData.y_cf,   featData.z_cf,
    featData.vrms, featData.kurtosis, featData.skewness
  );

  // 4. Gửi ESP-NOW
  esp_now_send(receiverAddress, (uint8_t *)&featData, sizeof(featData));

  // 5. Chờ ngắn để ESP-NOW xử lý xong trước chu kỳ tiếp theo
  delay(50);
}
