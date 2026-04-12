// IchigoJam_Z - GPIO / ADC / I2C / PWM HAL for Zephyr

#ifndef __ICHIGOJAM_IO_H__
#define __ICHIGOJAM_IO_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/i2c.h>

// --- Pin assignments (Raspberry Pi Pico) ---
// OUT1-OUT4: GPIO8-11, OUT5: GPIO22, OUT6: GPIO21
// IN1: GPIO27(ADC ch1), IN2: GPIO26(ADC ch0), IN3: GPIO6, IN4: GPIO7
// LED: GPIO25 (onboard), BTN: GPIO28 (ADC ch2)

#define PIN_OUT1  8
#define PIN_OUT2  9
#define PIN_OUT3  10
#define PIN_OUT4  11
#define PIN_OUT5  22
#define PIN_OUT6  21
#define PIN_IN1   27
#define PIN_IN2   26
#define PIN_IN3   6
#define PIN_IN4   7
#define PIN_LED   25
#define PIN_BTN   28

#define IO_PIN_NUM 11

// out_pins[0..10] = OUT1..OUT6, LED, IN1, IN2, IN3, IN4
// index 0 = OUT1(port 1) .. index 6 = LED(port 7)
static const uint8_t out_pins[IO_PIN_NUM] = {
    PIN_OUT1, PIN_OUT2, PIN_OUT3, PIN_OUT4,
    PIN_OUT5, PIN_OUT6, PIN_LED,
    PIN_IN1,  PIN_IN2,  PIN_IN3,  PIN_IN4
};

// in_pins[0..10] = IN1..IN4, OUT1..OUT4, BTN, OUT5, OUT6
static const uint8_t in_pins[IO_PIN_NUM] = {
    PIN_IN1,  PIN_IN2,  PIN_IN3,  PIN_IN4,
    PIN_OUT1, PIN_OUT2, PIN_OUT3, PIN_OUT4,
    PIN_BTN,  PIN_OUT5, PIN_OUT6
};

#define ANA_THRESHOLD (1024 / 4)
#define PLEN_MAX 2000

#define GPIO_DEV DEVICE_DT_GET(DT_NODELABEL(gpio0))
#define ADC_DEV  DEVICE_DT_GET(DT_NODELABEL(adc))

// ADC read: channel 0-2, returns 10-bit value (0-1023)
// RP2040 ADC is 12-bit; shift >>2 for 10-bit.
// gpio26=ch0(IN2), gpio27=ch1(IN1), gpio28=ch2(BTN)
static int _adc_read(int channel)
{
    const struct device *dev = ADC_DEV;
    int16_t buf = 0;
    struct adc_sequence seq = {
        .channels    = BIT(channel),
        .buffer      = &buf,
        .buffer_size = sizeof(buf),
        .resolution  = 12,
    };
    if (adc_read(dev, &seq) < 0) {
        return 0;
    }
    if (buf < 0) buf = 0;
    return (int)buf >> 2; // 12-bit -> 10-bit
}

void io_init(void)
{
    const struct device *gpio = GPIO_DEV;
    const struct device *adc  = ADC_DEV;

    if (!device_is_ready(gpio)) {
        return;
    }

    // IN1-IN4: input with pull-up (includes ADC-capable GPIO26/27)
    gpio_pin_configure(gpio, PIN_IN1, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(gpio, PIN_IN2, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(gpio, PIN_IN3, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_configure(gpio, PIN_IN4, GPIO_INPUT | GPIO_PULL_UP);

    // BTN: input with pull-up (GPIO28)
    gpio_pin_configure(gpio, PIN_BTN, GPIO_INPUT | GPIO_PULL_UP);

    // OUT1..OUT6: output low
    for (int i = 1; i <= 6; i++) {
        IJB_out(i, 0);
    }

    // LED: output low
    gpio_pin_configure(gpio, PIN_LED, GPIO_OUTPUT_INACTIVE);

    // ADC channel setup (IJB_ana uses these; pull-up bias is acceptable)
    if (device_is_ready(adc)) {
        struct adc_channel_cfg cfg = {
            .gain             = ADC_GAIN_1,
            .reference        = ADC_REF_INTERNAL,
            .acquisition_time = ADC_ACQ_TIME_DEFAULT,
        };
        for (int ch = 0; ch <= 2; ch++) {
            cfg.channel_id = ch;
            adc_channel_setup(adc, &cfg);
        }
    }
}

int i2c0_init(void) {
    return 0;
}

// io_get(): read all IN pins as digital GPIO (with pull-up, same as P version)
S_INLINE int io_get(void)
{
    const struct device *gpio = GPIO_DEV;
    int res = 0;
    for (int i = 0; i < IO_PIN_NUM; i++) {
        if (gpio_pin_get(gpio, in_pins[i]) > 0) {
            res |= (1 << i);
        }
    }
    return res;
}

int IJB_in(void) {
    return io_get();
}

void IJB_out(int port, int st)
{
    const struct device *gpio = GPIO_DEV;
    if (port < 0 || port > IO_PIN_NUM) {
        return;
    }
    if (port == 0) {
        for (int i = 0; i < IO_PIN_NUM; i++) {
            gpio_pin_configure(gpio, out_pins[i], GPIO_OUTPUT);
            gpio_pin_set(gpio, out_pins[i], (st >> i) & 1);
        }
    } else {
        uint8_t pin = out_pins[port - 1];
        if (st >= 0) {
            gpio_pin_configure(gpio, pin, GPIO_OUTPUT);
            gpio_pin_set(gpio, pin, st ? 1 : 0);
        } else if (st == -1) {
            // Input float
            gpio_pin_configure(gpio, pin, GPIO_INPUT);
        } else if (st == -2) {
            // Input pull-up
            gpio_pin_configure(gpio, pin, GPIO_INPUT | GPIO_PULL_UP);
        }
    }
}

S_INLINE void IJB_led(int st) {
    IJB_out(7, st != 0);
}

// BTN(): read GPIO28 as digital (pull-up, GND = pressed = 1)
int IJB_btn(int n)
{
    if (n == 0) {
        return (gpio_pin_get(GPIO_DEV, PIN_BTN) == 0) ? 1 : 0;
    }
    return 0;
}

S_INLINE int IJB_ana(int n)
{
    // n=1 or n=9 -> IN1 (GPIO27, ADC ch1)
    // n=2        -> IN2 (GPIO26, ADC ch0)
    int ch;
    if (n == 1 || n == 9) {
        ch = 1;
    } else if (n == 2) {
        ch = 0;
    } else {
        return 0;
    }
    return _adc_read(ch);
}

S_INLINE void IJB_clo(void) {
    io_init();
}

// --- PWM stub (implemented in M3) ---

void IJB_pwm(int port, int plen, int len) {
}

S_INLINE void pwm_off(int port) {
}

// --- I2C stub (implemented in M4) ---

S_INLINE int IJB_i2c(uint8 writemode, uint16 *param) {
    return 1; // io error (not implemented yet)
}

#endif // __ICHIGOJAM_IO_H__
