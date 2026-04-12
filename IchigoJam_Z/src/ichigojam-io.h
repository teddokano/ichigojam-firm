// IchigoJam_Z - GPIO / ADC / I2C / PWM HAL for Zephyr
//
// GPIO pin assignments are defined per-board in the DT overlay
// via the zephyr,user node (ij-led-gpios, ij-out1-gpios, ...).
// This file is board-agnostic.

#ifndef __ICHIGOJAM_IO_H__
#define __ICHIGOJAM_IO_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/i2c.h>

// --- GPIO specs from DT overlay (zephyr,user node) ---
//
// out_spec[0..10]: OUT1..OUT6, LED, IN1..IN4
//   index 0=OUT1 .. 5=OUT6, 6=LED, 7=IN1 .. 10=IN4
// in_spec[0..10]:  IN1..IN4, OUT1..OUT4, BTN, OUT5, OUT6
//   bit layout: bit0=IN1..bit3=IN4, bit4=OUT1..bit7=OUT4, bit8=BTN, bit9=OUT5, bit10=OUT6

#define _IJ DT_PATH(zephyr_user)

static const struct gpio_dt_spec _out_spec[] = {
    GPIO_DT_SPEC_GET(_IJ, ij_out1_gpios),  /* [0] OUT1 */
    GPIO_DT_SPEC_GET(_IJ, ij_out2_gpios),  /* [1] OUT2 */
    GPIO_DT_SPEC_GET(_IJ, ij_out3_gpios),  /* [2] OUT3 */
    GPIO_DT_SPEC_GET(_IJ, ij_out4_gpios),  /* [3] OUT4 */
    GPIO_DT_SPEC_GET(_IJ, ij_out5_gpios),  /* [4] OUT5 */
    GPIO_DT_SPEC_GET(_IJ, ij_out6_gpios),  /* [5] OUT6 */
    GPIO_DT_SPEC_GET(_IJ, ij_led_gpios),   /* [6] LED  */
    GPIO_DT_SPEC_GET(_IJ, ij_in1_gpios),   /* [7] IN1  */
    GPIO_DT_SPEC_GET(_IJ, ij_in2_gpios),   /* [8] IN2  */
    GPIO_DT_SPEC_GET(_IJ, ij_in3_gpios),   /* [9] IN3  */
    GPIO_DT_SPEC_GET(_IJ, ij_in4_gpios),   /* [10] IN4 */
};
#define IO_PIN_NUM 11

// in_spec order: IN1..IN4, OUT1..OUT4, BTN, OUT5, OUT6
static const struct gpio_dt_spec _in_spec[] = {
    GPIO_DT_SPEC_GET(_IJ, ij_in1_gpios),   /* bit 0:  IN1  */
    GPIO_DT_SPEC_GET(_IJ, ij_in2_gpios),   /* bit 1:  IN2  */
    GPIO_DT_SPEC_GET(_IJ, ij_in3_gpios),   /* bit 2:  IN3  */
    GPIO_DT_SPEC_GET(_IJ, ij_in4_gpios),   /* bit 3:  IN4  */
    GPIO_DT_SPEC_GET(_IJ, ij_out1_gpios),  /* bit 4:  OUT1 */
    GPIO_DT_SPEC_GET(_IJ, ij_out2_gpios),  /* bit 5:  OUT2 */
    GPIO_DT_SPEC_GET(_IJ, ij_out3_gpios),  /* bit 6:  OUT3 */
    GPIO_DT_SPEC_GET(_IJ, ij_out4_gpios),  /* bit 7:  OUT4 */
    GPIO_DT_SPEC_GET(_IJ, ij_btn_gpios),   /* bit 8:  BTN  */
    GPIO_DT_SPEC_GET(_IJ, ij_out5_gpios),  /* bit 9:  OUT5 */
    GPIO_DT_SPEC_GET(_IJ, ij_out6_gpios),  /* bit 10: OUT6 */
};

static const struct gpio_dt_spec _btn_spec =
    GPIO_DT_SPEC_GET(_IJ, ij_btn_gpios);

#define ANA_THRESHOLD (1024 / 4)
#define PLEN_MAX 2000

// --- ADC (RP2040: "adc", MCXA153: "lpadc0") ---
#if DT_NODE_EXISTS(DT_NODELABEL(adc))
#  define ADC_DEV     DEVICE_DT_GET(DT_NODELABEL(adc))
#  define ADC_ENABLED 1
#elif DT_NODE_EXISTS(DT_NODELABEL(lpadc0))
#  define ADC_DEV     DEVICE_DT_GET(DT_NODELABEL(lpadc0))
#  define ADC_ENABLED 1
#else
#  define ADC_ENABLED 0
#endif

// ADC read: channel 0-2, returns 10-bit value (0-1023).
// RP2040: 12-bit ADC, shift >>2. gpio26=ch0, gpio27=ch1, gpio28=ch2.
static int _adc_read(int channel)
{
#if ADC_ENABLED
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
#else
    return 0;
#endif
}

void io_init(void)
{
    // IN1-IN4: input with pull-up
    for (int i = 0; i < 4; i++) {
        gpio_pin_configure_dt(&_out_spec[7 + i], GPIO_INPUT | GPIO_PULL_UP);
    }
    // BTN: input with pull-up
    gpio_pin_configure_dt(&_btn_spec, GPIO_INPUT | GPIO_PULL_UP);
    // OUT1-OUT6: output inactive
    for (int i = 1; i <= 6; i++) {
        IJB_out(i, 0);
    }
    // LED: output inactive (gpio_dt_spec handles active-low inversion)
    gpio_pin_configure_dt(&_out_spec[6], GPIO_OUTPUT_INACTIVE);

#if ADC_ENABLED
    {
        const struct device *adc = ADC_DEV;
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
#endif
}

int i2c0_init(void) {
    return 0;
}

// io_get(): raw physical bitmask (same convention as P version)
// Uses gpio_pin_get_raw() so active-low flags don't invert the result.
S_INLINE int io_get(void)
{
    int res = 0;
    for (int i = 0; i < IO_PIN_NUM; i++) {
        if (gpio_pin_get_raw(_in_spec[i].port, _in_spec[i].pin) > 0) {
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
    if (port < 0 || port > IO_PIN_NUM) {
        return;
    }
    if (port == 0) {
        for (int i = 0; i < IO_PIN_NUM; i++) {
            gpio_pin_configure_dt(&_out_spec[i], GPIO_OUTPUT);
            gpio_pin_set_dt(&_out_spec[i], (st >> i) & 1);
        }
    } else {
        const struct gpio_dt_spec *s = &_out_spec[port - 1];
        if (st >= 0) {
            gpio_pin_configure_dt(s, GPIO_OUTPUT);
            gpio_pin_set_dt(s, st ? 1 : 0);
        } else if (st == -1) {
            gpio_pin_configure_dt(s, GPIO_INPUT);
        } else if (st == -2) {
            gpio_pin_configure_dt(s, GPIO_INPUT | GPIO_PULL_UP);
        }
    }
}

// LED: port 7 in out_spec. gpio_pin_set_dt handles active-low automatically.
S_INLINE void IJB_led(int st) {
    IJB_out(7, st != 0);
}

// BTN: gpio_pin_get_dt returns 1 when active (pressed), handles active-low.
int IJB_btn(int n)
{
    if (n == 0) {
        return gpio_pin_get_dt(&_btn_spec);
    }
    return 0;
}

S_INLINE int IJB_ana(int n)
{
    // n=1 or n=9 -> IN1 (RP2040: GPIO27=ADC ch1)
    // n=2        -> IN2 (RP2040: GPIO26=ADC ch0)
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
    return 1;
}

#endif // __ICHIGOJAM_IO_H__
