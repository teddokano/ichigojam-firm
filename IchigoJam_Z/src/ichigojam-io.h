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
#include <zephyr/drivers/pwm.h>

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

// --- ADC ---
//
// Two paths depending on the board overlay:
//
//   ADC_DT path  (MCXN947): zephyr,user has io-channels property.
//     Channel specs come from DT; adc_channel_setup_dt() / adc_read().
//     index 0 = IN1 (ANA(1)), index 1 = IN2 (ANA(2)).
//
//   Legacy path (RP2040, MCXA153): programmatic adc_channel_setup()
//     with raw channel IDs baked into the code.
//
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
// DT-based ADC: channel specs sourced from overlay io-channels property
static const struct adc_dt_spec _adc_dt[] = {
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0), /* IN1: ANA(1) */
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1), /* IN2: ANA(2) */
};
#define ADC_DT_ENABLED 1
#define ADC_ENABLED    0

#elif DT_NODE_EXISTS(DT_NODELABEL(adc))
#  define ADC_DEV      DEVICE_DT_GET(DT_NODELABEL(adc))
#  define ADC_ENABLED  1
#  define ADC_DT_ENABLED 0

#elif DT_NODE_EXISTS(DT_NODELABEL(lpadc0))
#  define ADC_DEV      DEVICE_DT_GET(DT_NODELABEL(lpadc0))
#  define ADC_ENABLED  1
#  define ADC_DT_ENABLED 0

#else
#  define ADC_ENABLED    0
#  define ADC_DT_ENABLED 0
#endif

// Legacy ADC read: channel 0-2, returns 10-bit value (0-1023).
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
    (void)channel;
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

#if ADC_DT_ENABLED
    for (int i = 0; i < (int)ARRAY_SIZE(_adc_dt); i++) {
        if (device_is_ready(_adc_dt[i].dev)) {
            adc_channel_setup_dt(&_adc_dt[i]);
        }
    }
#elif ADC_ENABLED
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
    // n=1 or n=9 → IN1 analog,  n=2 → IN2 analog
#if ADC_DT_ENABLED
    int idx;
    if (n == 1 || n == 9) {
        idx = 0; /* _adc_dt[0]: IN1 */
    } else if (n == 2) {
        idx = 1; /* _adc_dt[1]: IN2 */
    } else {
        return 0;
    }
    if (!device_is_ready(_adc_dt[idx].dev)) {
        return 0;
    }
    int16_t buf = 0;
    struct adc_sequence seq;
    adc_sequence_init_dt(&_adc_dt[idx], &seq);
    seq.buffer      = &buf;
    seq.buffer_size = sizeof(buf);
    if (adc_read(_adc_dt[idx].dev, &seq) < 0) {
        return 0;
    }
    if (buf < 0) buf = 0;
    return (int)buf >> 2; /* 12-bit → 10-bit */
#else
    // n=1 or n=9 -> ch=1, n=2 -> ch=0 (legacy channel mapping)
    int ch;
    if (n == 1 || n == 9) {
        ch = 1;
    } else if (n == 2) {
        ch = 0;
    } else {
        return 0;
    }
    return _adc_read(ch);
#endif
}

S_INLINE void IJB_clo(void) {
    io_init();
}

// --- PWM output on OUT1-5 ---
//
// PWM port,plen{,len}
//   port : 1-5  →  OUT1-OUT5
//   plen : pulse width 0-2000 (0.01 ms units;  1000 = 1 ms)
//   len  : period (default/0 → 2000 = 20 ms = 50 Hz, suits servo motors)
//
// Implementation note (RP2040):
//   Zephyr has no public API to switch a GPIO pin's mux to PWM at runtime.
//   We write directly to IO_BANK0 GPIO_CTRL[n].FUNCSEL (4=PWM, 5=SIO).
//   pwm_off() / IJB_out() restore FUNCSEL to SIO before reconfiguring GPIO.
//   RP2040 PWM channel number = gpio_pin % 16 (slices repeat every 16 pins).
//
//     OUT1=GPIO8  → ch 8    OUT2=GPIO9  → ch 9
//     OUT3=GPIO10 → ch 10   OUT4=GPIO11 → ch 11
//     OUT5=GPIO22 → ch 6   (22 % 16 = 6)

#if DT_NODE_EXISTS(DT_NODELABEL(pwm)) && defined(CONFIG_SOC_SERIES_RP2040)

#define _RP2040_IO_BANK0 0x40014000UL
#define _IJ_FUNC_PWM     4U
#define _IJ_FUNC_SIO     5U

static void _ij_gpio_func(uint32_t pin, uint32_t func)
{
    /* IO_BANK0: GPIO_STATUS at base+pin*8, GPIO_CTRL at base+pin*8+4 */
    volatile uint32_t *ctrl =
        (volatile uint32_t *)(_RP2040_IO_BANK0 + pin * 8U + 4U);
    *ctrl = (*ctrl & ~0x1FU) | (func & 0x1FU);
}

void IJB_pwm(int port, int plen, int len)
{
    if (port < 1 || port > 6) return;
    if (plen < 0)        plen = 0;
    if (plen > PLEN_MAX) plen = PLEN_MAX;
    if (len  <= 0)       len  = PLEN_MAX;  /* 2000 × 0.01 ms = 20 ms */

    /* Convert 0.01 ms units to nanoseconds */
    uint32_t period_ns = (uint32_t)len  * 10000U;
    uint32_t pulse_ns  = (uint32_t)plen * 10000U;
    if (pulse_ns > period_ns) pulse_ns = period_ns;

    const struct device *pwm_dev = DEVICE_DT_GET(DT_NODELABEL(pwm));
    if (!device_is_ready(pwm_dev)) return;

    uint32_t pin = (uint32_t)_out_spec[port - 1].pin;
    uint32_t ch  = pin % 16U;  /* RP2040 PWM slice mapping */

    pwm_set(pwm_dev, ch, period_ns, pulse_ns, PWM_POLARITY_NORMAL);
    _ij_gpio_func(pin, _IJ_FUNC_PWM);  /* switch pin mux to PWM function */
}

S_INLINE void pwm_off(int port)
{
    if (port < 1 || port > 6) return;
    /* Restore SIO function so gpio_pin_configure_dt() takes effect */
    _ij_gpio_func((uint32_t)_out_spec[port - 1].pin, _IJ_FUNC_SIO);
    IJB_out(port, 0);
}

#elif DT_NODE_HAS_STATUS(DT_NODELABEL(flexpwm0_pwm0), okay) && \
      DT_NODE_HAS_STATUS(DT_NODELABEL(flexpwm0_pwm1), okay) && \
      DT_NODE_HAS_STATUS(DT_NODELABEL(flexpwm0_pwm2), okay)

// frdm_mcxa153: dedicated FlexPWM0 pins, separate from OUT GPIO.
//   port 1,2 → flexpwm0_pwm0 (sm0) ch 0,1 → P3_6,  P3_7
//   port 3,4 → flexpwm0_pwm1 (sm1) ch 0,1 → P3_8,  P3_9
//   port 5,6 → flexpwm0_pwm2 (sm2) ch 0,1 → P3_10, P3_11

static const struct device * const _ij_flexpwm[] = {
    DEVICE_DT_GET(DT_NODELABEL(flexpwm0_pwm0)),  /* port 1,2 */
    DEVICE_DT_GET(DT_NODELABEL(flexpwm0_pwm1)),  /* port 3,4 */
    DEVICE_DT_GET(DT_NODELABEL(flexpwm0_pwm2)),  /* port 5,6 */
};

void IJB_pwm(int port, int plen, int len)
{
    if (port < 1 || port > 6) return;
    if (plen < 0)        plen = 0;
    if (plen > PLEN_MAX) plen = PLEN_MAX;
    if (len  <= 0)       len  = PLEN_MAX;

    uint32_t period_ns = (uint32_t)len  * 10000U;
    uint32_t pulse_ns  = (uint32_t)plen * 10000U;
    if (pulse_ns > period_ns) pulse_ns = period_ns;

    int dev_idx = (port - 1) / 2;
    uint32_t ch = (uint32_t)(port - 1) % 2U;

    const struct device *dev = _ij_flexpwm[dev_idx];
    if (!device_is_ready(dev)) return;
    pwm_set(dev, ch, period_ns, pulse_ns, PWM_POLARITY_NORMAL);
}

S_INLINE void pwm_off(int port)
{
    if (port < 1 || port > 6) return;
    int dev_idx = (port - 1) / 2;
    uint32_t ch = (uint32_t)(port - 1) % 2U;
    const struct device *dev = _ij_flexpwm[dev_idx];
    if (!device_is_ready(dev)) return;
    pwm_set(dev, ch, 10000000U, 0U, PWM_POLARITY_NORMAL);
}

#elif DT_NODE_HAS_STATUS(DT_NODELABEL(flexpwm1_pwm1), okay) && \
      DT_NODE_HAS_STATUS(DT_NODELABEL(flexpwm1_pwm2), okay)

// frdm_mcxn947: FlexPWM1 sm1/sm2 for OUT PWM; sm0 is reserved for sound.
//   port 1,2 → flexpwm1_pwm1 (sm1) ch 0,1 → PIO2_4, PIO2_5
//   port 3,4 → flexpwm1_pwm2 (sm2) ch 0,1 → PIO2_2, PIO2_3
//   port 5,6 → no dedicated PWM channel on this board (stubbed)

static const struct device * const _ij_flexpwm[] = {
    DEVICE_DT_GET(DT_NODELABEL(flexpwm1_pwm1)),  /* port 1,2 */
    DEVICE_DT_GET(DT_NODELABEL(flexpwm1_pwm2)),  /* port 3,4 */
};

void IJB_pwm(int port, int plen, int len)
{
    if (port < 1 || port > 6) return;
    if (port > 4) return;  /* OUT5/6: no PWM on MCXN947, silently ignore */
    if (plen < 0)        plen = 0;
    if (plen > PLEN_MAX) plen = PLEN_MAX;
    if (len  <= 0)       len  = PLEN_MAX;

    uint32_t period_ns = (uint32_t)len  * 10000U;
    uint32_t pulse_ns  = (uint32_t)plen * 10000U;
    if (pulse_ns > period_ns) pulse_ns = period_ns;

    int dev_idx = (port - 1) / 2;  /* 0=sm1(port1,2), 1=sm2(port3,4) */
    uint32_t ch = (uint32_t)(port - 1) % 2U;

    const struct device *dev = _ij_flexpwm[dev_idx];
    if (!device_is_ready(dev)) return;
    pwm_set(dev, ch, period_ns, pulse_ns, PWM_POLARITY_NORMAL);
}

S_INLINE void pwm_off(int port)
{
    if (port < 1 || port > 4) return;
    int dev_idx = (port - 1) / 2;
    uint32_t ch = (uint32_t)(port - 1) % 2U;
    const struct device *dev = _ij_flexpwm[dev_idx];
    if (!device_is_ready(dev)) return;
    pwm_set(dev, ch, 10000000U, 0U, PWM_POLARITY_NORMAL);
}

#else

/* No supported PWM hardware: stub */
void IJB_pwm(int port, int plen, int len) { (void)port; (void)plen; (void)len; }
S_INLINE void pwm_off(int port) { (void)port; }

#endif /* PWM implementations */

// --- I2C stub (implemented in M4) ---
S_INLINE int IJB_i2c(uint8 writemode, uint16 *param) {
    return 1;
}

#endif // __ICHIGOJAM_IO_H__
