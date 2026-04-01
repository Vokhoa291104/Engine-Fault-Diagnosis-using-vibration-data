/*
 * ESP-IDF Main Application Logic
 *
 * This file contains the high-level application logic:
 * 1. WiFi Connection
 * 2. MQTT Connection
 * 3. The main `sensor_node_task` which uses the sensor component.
 *
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"


// Include the public header for sensor component
#include "adxl345.h" 

static const char *TAG = "MAIN_APP";

// -------------------------------------------------------------------
// --- 1. NETWORK CONFIGURATION ---
// -------------------------------------------------------------------
#define WIFI_SSID                   "WIFI_SSID"
#define WIFI_PASSWORD               "WIFI_PASSWORD"
#define MQTT_BROKER_URL             "mqtt://YOUR_PI_URL"
#define MQTT_TOPIC                  "v1/devices/me/telemetry"

// -------------------------------------------------------------------
// --- 2. GLOBAL VARIABLES & NETWORK HANDLERS ---
// -------------------------------------------------------------------
static bool wifi_connected = false;
static bool mqtt_connected = false;
static esp_mqtt_client_handle_t client;

// WiFi Event Handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected");
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
        wifi_connected = false;
    } else if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

// MQTT Event Handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            mqtt_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            mqtt_connected = false;
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle) {
                ESP_LOGE(TAG, "Last error code: 0x%x, error type: 0x%x",
                         event->error_handle->error_type,
                         event->error_handle->connect_return_code);
            }
            break;
        default:
            ESP_LOGI(TAG, "Other MQTT event id: %ld", event_id);
            break;
    }
}

// -------------------------------------------------------------------
// --- 3. MAIN SENSOR TASK ---
// -------------------------------------------------------------------

static void sensor_node_task(void *pvParameters) {
    
    // 1. Initialize the sensor
    // This single function now handles power, I2C, and sensor config.
    if (sensor_adxl345_init() != ESP_OK) {
        ESP_LOGE(TAG, "Sensor initialization failed. Halting task.");
        vTaskDelete(NULL);
        return;
    }

    // 2. Calibrate the sensor
    // This runs the calibration logic from the component.
    ESP_LOGI(TAG, "Calibrating sensor... Please keep it still.");
    sensor_adxl345_calibrate();
    ESP_LOGI(TAG, "Calibration complete.");

    // This is the 20-byte struct defined in the component's .h file
    SensorData data; 
    
    while (1) {
        // Wait for network connection
        if (!wifi_connected || !mqtt_connected) {
            ESP_LOGW(TAG, "Waiting for network connection...");
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        // 3. Read calibrated data
        // This single function now reads, converts, and applies offsets.
        if (sensor_adxl345_read_calibrated_data(&data) == ESP_OK) {
            
            // Log the calibrated data
            ESP_LOGI(TAG, "Calibrated Data - X:%.2f | Y:%.2f | Z:%.2f | Vrms:%.2f",
                     data.x, data.y, data.z, data.vrms);

            // 4. Publish the data
            esp_mqtt_client_publish(client,
                                    MQTT_TOPIC,
                                    (const char *)&data,
                                    sizeof(SensorData), // 20 bytes
                                    1, // QoS
                                    0); // Retain
        } else {
            ESP_LOGE(TAG, "Failed to read sensor data");
        }

        // Wait for 4 seconds
        vTaskDelay(4000 / portTICK_PERIOD_MS);
    }
}

// -------------------------------------------------------------------
// --- 4. NETWORK & APP INIT ---
// -------------------------------------------------------------------
static void network_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization complete.");
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting Sensor Node Application");

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize network (WiFi and MQTT)
    network_init();
    mqtt_app_start();

    // Start the main sensor task
    xTaskCreate(sensor_node_task, "sensor_node_task", 4096, NULL, 5, NULL);
}
