# HỆ THỐNG CHUẨN ĐOÁN LỖI ĐỘNG CƠ DỰA TRÊN DỮ LIỆU RUNG KẾT HỢP MQTT 

This project captures real-time vibration data using an ADXL345 accelerometer connected to an ESP32-C3. The ESP32 node performs on-device calibration, processes the data, and sends it as an efficient binary packet via MQTT.
A Raspberry Pi acts as a gateway, subscribing to the MQTT topic, unpacking the binary data, adding a real-time timestamp, and forwarding the data as JSON to a ThingsBoard dashboard for visualization and storage.

## System Architecture

The data flows from the sensor to the dashboard in four main stages:

[ADXL345 Sensor] → (I2C) → [ESP32-C3] → (WiFi / MQTT) → [Raspberry Pi Gateway] → (HTTP POST) → [ThingsBoard Server]

Sensor (ADXL345): Measures raw acceleration on 3 axes.

Sensor Node (ESP32-C3):

- Powers the sensor using GPIOs.

- Initializes the sensor over I2C.

- Performs startup calibration to zero-out static gravity.

- Calculates calibrated X, Y, Z, and Vrms values.

- Packs data into a 16-byte binary struct.

- Connects to WiFi and sends the struct to an MQTT topic.

Gateway (Raspberry Pi):

- Runs a local Mosquitto MQTT broker.

- Runs a Python script that subscribes to the MQTT topic.

- Unpacks the 16-byte struct.

- Generates a real-time Unix timestamp (in milliseconds).

- Formats the data into a ThingsBoard JSON payload.

- Sends the JSON payload via HTTP POST to the ThingsBoard server.

Dashboard (ThingsBoard):

- Receives the JSON data.

- Stores and visualizes the telemetry (X, Y, Z, Vrms) in real-time.
```
IoT-Vibration-Detect/
├── main/
│   ├── main.c     # Main application (WiFi, MQTT, main task)
│   └── CMakeLists.txt     # Main component build script
│
├── components/
│   └── adxl345/    # Reusable sensor component
│       ├── adxl345.c # Sensor driver logic (power, I2C, calibration)
│       ├── CMakeLists.txt   # Component build script
│       └── include/
│           └── adxl345.h # Public header (SensorData struct, functions)
│
├── pi_gateway.py            # Python script for the Raspberry Pi
├── sdkconfig                # ESP-IDF project configuration
└── README.md                # This file
```
### Hardware Connections
| ESP32-C3  | ADXL345 | Description           |
| :-------- | :------ | :-------------------- | 
| GND       | GPIO 5  | Sensor Ground         |
| VCC       | GPIO 6  | Sensor 3.3V Power     |
| SDA       | GPIO 20 | I2C Data              |
| SCL       | GPIO 21 | I2C Clock             |
