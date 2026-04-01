/*
 * Public API Header for the ADXL345 Sensor Component
 *
 * This file defines the public data structures and functions
 * that the main application can use.
 */

#ifndef ADXL345_H
#define ADXL345_H

#include "esp_err.h" // For esp_err_t
#include "stdint.h"  // For uint32_t

/**
 * @brief The 20-byte data structure for sensor readings.
 * * This struct is packed to ensure it is exactly 20 bytes
 * and matches the Python gateway's expected format ('ffffL').
 */
typedef struct __attribute__((packed)) {
    float x;    // Calibrated m/s^2
    float y;    // Calibrated m/s^2
    float z;    // Calibrated m/s^2
    float vrms; // Calculated Vrms
    uint32_t timestamp_ms; // 4-byte timestamp
} SensorData; // Total size: 4*4 + 4 = 20 bytes

/**
 * @brief Initializes the ADXL345 sensor.
 * * This function handles:
 * 1. Turning on sensor power via GPIO pins.
 * 2. Initializing the I2C driver.
 * 3. Configuring the sensor registers (range, resolution, power).
 *
 * @return 
 * - ESP_OK on success
 * - ESP_FAIL or other error codes on failure
 */
esp_err_t sensor_adxl345_init(void);

/**
 * @brief Calibrates the sensor by calculating gravity offsets.
 * * Reads a number of samples while at rest to determine the
 * average pull of gravity on each axis. These offsets are
 * stored internally and subtracted from all future readings.
 *
 * NOTE: The sensor MUST be still during this process.
 */
void sensor_adxl345_calibrate(void);

/**
 * @brief Reads the current sensor data, calibrates it, and fills the struct.
 *
 * @param[out] data A pointer to the SensorData struct to be filled.
 *
 * @return 
 * - ESP_OK on success
 * - ESP_FAIL if I2C read fails
 */
esp_err_t sensor_adxl345_read_calibrated_data(SensorData *data);

#endif //ADXL345_H
