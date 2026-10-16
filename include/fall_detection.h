#ifndef FALL_DETECTION_H
#define FALL_DETECTION_H

#include <Arduino.h>
#include <board_pins.h>

constexpr gpio_num_t FALL_INT_PIN = Board::accelInt1;

// Suspected fall: low acceleration followed by impact, not a validated human classifier.
// ADXL362: +/-8g (4 mg/LSB), 50 Hz (20 ms/sample).
constexpr uint16_t FALL_THRESHOLD_MG = 375;
constexpr uint16_t FALL_SAMPLE_MS = 20;
constexpr uint16_t FALL_DURATION_SAMPLES = 8; // 160 ms
constexpr uint16_t FALL_IMPACT_MG = 3000;
constexpr uint16_t FALL_IMPACT_WINDOW_MS = 1000;

bool setup_fall_detection(bool debug = false);
void update_fall_detection(bool debug);
bool fall_detection_pending();
void acknowledge_fall_detection();
// Leaves the sensor measuring; refuses sleep if an event/read failure is pending.
bool prepare_fall_detection_sleep();

#endif
