/*
 * Implementation file for the ADXL345 Sensor Component
 *
 * This file contains all the private, low-level logic for
 * handling the sensor.
 */

#include <math.h>
#include "adxl345.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ADXL345_SENSOR";

// -------------------------------------------------------------------
// --- 1. PIN & SENSOR CONFIGURATION ---
// -------------------------------------------------------------------

// Sensor Power Pins
#define SENSOR_POWER_PIN_GND        5
#define SENSOR_POWER_PIN_VCC        6

// I2C Communication Pins
#define I2C_MASTER_SCL_IO           21
#define I2C_MASTER_SDA_IO           20

// I2C General Config
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000

// ADXL345 Specifics
#define ADXL345_I2C_ADDRESS         0x53
#define ADXL345_REG_DATA_FORMAT     0x31
#define ADXL345_REG_POWER_CTL       0x2D
#define ADXL345_REG_DATAX0          0x32

// Sensor settings (2G-4G-8G-16G range, full resolution)
#define ADXL345_RANGE_2G            0x00
#define ADXL345_RANGE_4G            0x01
#define ADXL345_RANGE_8G            0x02
#define ADXL345_RANGE_16G           0x03
#define ADXL345_FULL_RES            0x08
#define ADXL345_MEASURE             0x08

// Conversion Constants
#define SENSORS_GRAVITY_STANDARD    (9.80665F)
// (3.9 mg/LSB at 10-bit full-res) * (1G / 1000mg) = 0.0039 G/LSB
#define ADXL345_CONVERSION_FACTOR   (0.00390625F)
#define CALIBRATION_SAMPLES         50 //50 or 100

// -------------------------------------------------------------------
// --- 2. INTERNAL STATE (Static Variables) ---
// -------------------------------------------------------------------

// Internal offsets, calculated during calibration
static float x_offset = 0.0;
static float y_offset = 0.0;
static float z_offset = 0.0;

// -------------------------------------------------------------------
// --- 3. LOW-LEVEL (Static) HELPER FUNCTIONS ---
// -------------------------------------------------------------------

/**
 * @brief Configure and turn on sensor power using GPIOs
 */
static esp_err_t sensor_power_init(void) {
    ESP_LOGI(TAG, "Configuring sensor power pins...");
    gpio_config_t conf = {
        .pin_bit_mask = (1ULL << SENSOR_POWER_PIN_GND) | (1ULL << SENSOR_POWER_PIN_VCC),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t ret = gpio_config(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure power GPIOs");
        return ret;
    }

    gpio_set_level(SENSOR_POWER_PIN_GND, 0); // Set to LOW (GND)
    gpio_set_level(SENSOR_POWER_PIN_VCC, 1); // Set to HIGH (3.3V)
    
    ESP_LOGI(TAG, "Sensor power ON (GND=%d, VCC=%d).", SENSOR_POWER_PIN_GND, SENSOR_POWER_PIN_VCC);
    
    // Give the sensor 100ms to power up
    vTaskDelay(100 / portTICK_PERIOD_MS); 
    return ESP_OK;
}

/**
 * @brief I2C master initialization
 */
static esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

/**
 * @brief Write a byte to an ADXL345 register
 */
static esp_err_t adxl345_write_reg(uint8_t reg, uint8_t data) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADXL345_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}

/**
 * @brief Read bytes from an ADXL345 register
 */
static esp_err_t adxl345_read_reg(uint8_t reg, uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ADXL345_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd); // Repeated start
    i2c_master_write_byte(cmd, (ADXL345_I2C_ADDRESS << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}

/**
 * @brief Helper to read 6-byte raw accel data
 */
static esp_err_t read_raw_accel(int16_t *x, int16_t *y, int16_t *z) {
    uint8_t raw_data[6];
    esp_err_t ret = adxl345_read_reg(ADXL345_REG_DATAX0, raw_data, 6);
    if (ret == ESP_OK) {
        *x = (int16_t)((raw_data[1] << 8) | raw_data[0]);
        *y = (int16_t)((raw_data[3] << 8) | raw_data[2]);
        *z = (int16_t)((raw_data[5] << 8) | raw_data[4]);
    }
    return ret;
}

// -------------------------------------------------------------------
// --- 4. PUBLIC API FUNCTIONS (Implementation) ---
// -------------------------------------------------------------------

esp_err_t sensor_adxl345_init(void) {
    // 1. Turn on power
    esp_err_t ret = sensor_power_init();
    if (ret != ESP_OK) {
        return ret;
    }

    // 2. Init I2C bus
    ret = i2c_master_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver initialization failed");
        return ret;
    }
    ESP_LOGI(TAG, "I2C driver initialized.");

    // 3. Configure sensor: 8G range, full resolution
    ret = adxl345_write_reg(ADXL345_REG_DATA_FORMAT, ADXL345_RANGE_8G | ADXL345_FULL_RES);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set data format (8G, Full-Res)");
        return ret;
    }

    // 4. Enable measurement
    ret = adxl345_write_reg(ADXL345_REG_POWER_CTL, ADXL345_MEASURE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable measurement");
        return ret;
    }
    
    ESP_LOGI(TAG, "ADXL345 configured successfully (8G Range, Full-Res).");
    return ESP_OK;
}

void sensor_adxl345_calibrate(void) {
    double x_sum = 0.0, y_sum = 0.0, z_sum = 0.0;
    int16_t raw_x, raw_y, raw_z;
    int samples_taken = 0;

    for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
        if (read_raw_accel(&raw_x, &raw_y, &raw_z) == ESP_OK) {
            x_sum += (float)raw_x;
            y_sum += (float)raw_y;
            z_sum += (float)raw_z;
            samples_taken++;
        }
        vTaskDelay(20 / portTICK_PERIOD_MS); // Wait 20ms
    }

    if (samples_taken > 0) {
        // Calculate average raw LSB value and convert to m/s^2 offset
        float lsb_to_ms2 = ADXL345_CONVERSION_FACTOR * SENSORS_GRAVITY_STANDARD;
        x_offset = (x_sum / samples_taken) * lsb_to_ms2;
        y_offset = (y_sum / samples_taken) * lsb_to_ms2;
        z_offset = (z_sum / samples_taken) * lsb_to_ms2;

        ESP_LOGI(TAG, "Calibration offsets (m/s^2): X:%.2f, Y:%.2f, Z:%.2f", x_offset, y_offset, z_offset);
    } else {
        ESP_LOGE(TAG, "Calibration failed, could not read sensor.");
    }
}

esp_err_t sensor_adxl345_read_calibrated_data(SensorData *data) {
    int16_t raw_x, raw_y, raw_z;
    
    esp_err_t ret = read_raw_accel(&raw_x, &raw_y, &raw_z);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // 1. Convert raw LSB values to m/s^2
    float current_x = (float)raw_x * ADXL345_CONVERSION_FACTOR * SENSORS_GRAVITY_STANDARD;
    float current_y = (float)raw_y * ADXL345_CONVERSION_FACTOR * SENSORS_GRAVITY_STANDARD;
    float current_z = (float)raw_z * ADXL345_CONVERSION_FACTOR * SENSORS_GRAVITY_STANDARD;

    // 2. Apply calibration offsets
    data->x = current_x - x_offset;
    data->y = current_y - y_offset;
    data->z = current_z - z_offset;
    
    // 3. Calculate Vrms from calibrated data
    data->vrms = sqrtf((data->x * data->x + data->y * data->y + data->z * data->z) / 3.0f);

    // 4. Get timestamp
    data->timestamp_ms = (uint32_t)esp_log_timestamp();

    return ESP_OK;
}
