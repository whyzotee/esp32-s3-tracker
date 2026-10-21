#include <board_pins.h>
#include <audio_feedback.h>
#include <driver/gpio.h>

namespace Board {
void setVext(bool enabled)
{
    gpio_set_level(gpio_num_t(vextControl), enabled ? LOW : HIGH);
    pinMode(vextControl, OUTPUT);
}

void begin()
{
    gpio_deep_sleep_hold_dis();
    // Preload output latches before releasing the levels held during sleep.
    setVext(false);
    gpio_hold_dis(gpio_num_t(vextControl));
    for (int pin : {buzzer, vibration, statusLed}) {
        gpio_set_level(gpio_num_t(pin), LOW);
        pinMode(pin, OUTPUT);
        gpio_hold_dis(gpio_num_t(pin));
    }
    // LED2 / CPU_LED is active-high and indicates the MCU is awake.
    digitalWrite(statusLed, HIGH);
}

void prepareSleep()
{
    setVext(false);
    gpio_hold_en(gpio_num_t(vextControl));
    AudioFeedback::prepareSleep();
    for (int pin : {buzzer, vibration, statusLed}) {
        digitalWrite(pin, LOW);
        gpio_hold_en(gpio_num_t(pin));
    }
    gpio_deep_sleep_hold_en();
}
}
