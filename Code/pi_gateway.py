# pi_gateway.py
#
# Raspberry Pi Gateway Script
# 1. Connects to a local MQTT broker.
# 2. Subscribes to the ESP32's raw data topic.
# 3. Unpacks the raw binary data.
# 4. Converts data to JSON.
# 5. Sends JSON to ThingsBoard.
#
# Required libraries:
# pip install paho-mqtt requests

import paho.mqtt.client as mqtt
import requests
import struct
import json
import time
import sys

# --- CONFIGURATION ---
MQTT_BROKER_HOST = "raspi"
MQTT_BROKER_PORT = 1883
MQTT_TOPIC = "v1/devices/me/telemetry"

THINGSBOARD_HOST = "https://demo.thingsboard.io/" # e.g., "http://demo.thingsboard.io"
THINGSBOARD_ACCESS_TOKEN = "cJcs9hr86fGePKyr4Gcx"
THINGSBOARD_URL = f"{THINGSBOARD_HOST}/api/v1/{THINGSBOARD_ACCESS_TOKEN}/telemetry"

# 'f' = 4-byte float
# 'ffff' = x, y, z, vrms
STRUCT_FORMAT = 'ffff' 
STRUCT_SIZE = struct.calcsize(STRUCT_FORMAT)

# Called when the client connects to the broker
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"Connected to MQTT Broker at {MQTT_BROKER_HOST}")
        client.subscribe(MQTT_TOPIC)
        print(f"Subscribed to topic: {MQTT_TOPIC}")
    else:
        print(f"Failed to connect, return code {rc}\n")

# Called when a message is received from the broker
def on_message(client, userdata, msg):
    print(f"Received message from topic: {msg.topic}")

    # 1. Check if payload size is correct
    if len(msg.payload) != STRUCT_SIZE:
        print(f"Error: Received payload size ({len(msg.payload)}) does not match expected size ({STRUCT_SIZE}).")
        return

    try:
        # 2. Unpack the raw binary data
        # data will be a tuple: (x, y, z, vrms)
        data = struct.unpack(STRUCT_FORMAT, msg.payload)
        
        #UNIX time
        timestamp_ms = int(time.time() * 1000)
        
        x, y, z, vrms = data

        print(f"  Unpacked Data: X={x:.2f}, Y={y:.2f}, Z={z:.2f}, Vrms={vrms:.2f}, T={timestamp_ms}")

        # 3. Convert data to JSON payload for ThingsBoard
        # ThingsBoard expects telemetry in a simple JSON dict.
        # It also accepts a 'ts' key for UNIX timestamps.
        payload = {
            "ts": timestamp_ms,
            "values": {
                "x_accel": x,
                "y_accel": y,
                "z_accel": z,
                "vrms": vrms
            }
        }
        
        # 4. Send JSON to ThingsBoard
        try:
            response = requests.post(THINGSBOARD_URL, json=payload, timeout=5)
            
            if response.status_code == 200:
                print(f"  Successfully sent data to ThingsBoard.")
            else:
                print(f"  Error sending to ThingsBoard. Status Code: {response.status_code}")
                print(f"  Response: {response.text}")
        
        except requests.exceptions.RequestException as e:
            print(f"  Error connecting to ThingsBoard: {e}")

    except struct.error as e:
        print(f"Error unpacking data: {e}")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")


def main():
    if THINGSBOARD_ACCESS_TOKEN == "YOUR_DEVICE_ACCESS_TOKEN":
        print("Error: Please update THINGSBOARD_HOST and THINGSBOARD_ACCESS_TOKEN in the script.")
        sys.exit(1)

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    try:
        client.connect(MQTT_BROKER_HOST, MQTT_BROKER_PORT, 60)
    except ConnectionRefusedError:
        print(f"Error: Connection to MQTT broker at {MQTT_BROKER_HOST}:{MQTT_BROKER_PORT} refused.")
        print("Is the Mosquitto broker running? (sudo systemctl status mosquitto)")
        sys.exit(1)

    # Loop forever, processing messages
    client.loop_forever()

if __name__ == "__main__":
    main()

