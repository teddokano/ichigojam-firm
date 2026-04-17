// IchigoJam_Z - GPIO / ADC / I2C / PWM HAL for Zephyr
//
// GPIO pin assignments are defined per-board in the DT overlay
// via the zephyr,user node (ij-led-gpios, ij-out1-gpios, ...).
// ADC, I2C, and PWM peripherals are selected via the DT chosen node
// (ij-adc, ij-i2c) or zephyr,user pwms properties (ij-sound-pwms,
// ij-pwm1-pwms..ij-pwm6-pwms) — no board-specific labels in this file.

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
// Device selected via: chosen { ij-adc = <&adc>; } (or &lpadc0, etc.)
// No board-specific label referenced here.
#if DT_HAS_CHOSEN(ij_adc)
#  define ADC_DEV     DEVICE_DT_GET(DT_CHOSEN(ij_adc))
#  define ADC_ENABLED 1
#else
#  define ADC_ENABLED 0
#endif

// ADC read: channel 0-based slot index, returns 10-bit value (0-1023).
// 12-bit ADC result is shifted >>2 to produce 10-bit output.
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

#if ADC_ENABLED
    {
        const struct device *adc = ADC_DEV;
        if (device_is_ready(adc)) {
#if DT_HAS_COMPAT_STATUS_OKAY(nxp_lpc_lpadc)
            /* NXP LPADC (MCXA153 etc.): external VDDA reference.
             * channel_id  = slot index 0-3.
             * input_positive = hardware ADC0_An number:
             *   slot0→A8(P1_10/A0), slot1→A10(P1_12/A1),
             *   slot2→A11(P1_13/A2), slot3→A0(P2_0/A3) */
            static const uint8_t mcxa_hw_ch[] = { 8, 10, 11, 0 };
            struct adc_channel_cfg cfg = {
                .gain             = ADC_GAIN_1,
                .reference        = ADC_REF_EXTERNAL0,
                .acquisition_time = ADC_ACQ_TIME_DEFAULT,
            };
            for (int i = 0; i < 4; i++) {
                cfg.channel_id     = i;
                cfg.input_positive = mcxa_hw_ch[i];
                adc_channel_setup(adc, &cfg);
            }
#else
            /* RP2040 and other boards: internal reference.
             * ch0=GPIO26(IN2), ch1=GPIO27(IN1), ch2=GPIO28(BTN) */
            struct adc_channel_cfg cfg = {
                .gain             = ADC_GAIN_1,
                .reference        = ADC_REF_INTERNAL,
                .acquisition_time = ADC_ACQ_TIME_DEFAULT,
            };
            for (int ch = 0; ch <= 2; ch++) {
                cfg.channel_id = ch;
                adc_channel_setup(adc, &cfg);
            }
#endif
        }
    }
#endif
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
#if DT_HAS_COMPAT_STATUS_OKAY(nxp_lpc_lpadc)
    /* NXP LPADC: ANA(n) → slot index 0-3 (set up in io_init).
     * ANA(1)/ANA(9)=slot0→A8(A0), ANA(2)=slot1→A10(A1),
     * ANA(3)=slot2→A11(A2),       ANA(4)=slot3→A0(A3)  */
    int slot = (n == 9) ? 0 : (n - 1);
    if (slot < 0 || slot > 3) return 0;
    return _adc_read(slot);
#else
    /* RP2040 and other boards: ANA(1)/ANA(9)=GPIO27=ch1, ANA(2)=GPIO26=ch0 */
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

// --- PWM output on OUT1-6 ---
//
// PWM port,plen{,len}
//   port : 1-6  →  OUT1-OUT6
//   plen : pulse width 0-2000 (0.01 ms units;  1000 = 1 ms)
//   len  : period (default/0 → 2000 = 20 ms = 50 Hz, suits servo motors)
//
// Per-board PWM pin/device mapping lives entirely in the DT overlay:
//   zephyr,user { ij-pwm1-pwms = <&dev ch period flags>; ... }
//
// RP2040 note: Zephyr has no public API to switch a GPIO pin's mux to PWM
// at runtime. We write directly to IO_BANK0 GPIO_CTRL[n].FUNCSEL (4=PWM).
// pwm_off() restores FUNCSEL=SIO before reconfiguring GPIO.
// This is scoped to CONFIG_SOC_SERIES_RP2040 and documented below.

// PWM OUT specs from overlay:
//   zephyr,user { pwms = <...>; pwm-names = "sound","out1",...,"out6"; }
// Accessed by name so board mapping stays entirely in the overlay.
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), pwms)

static const struct pwm_dt_spec _pwm_out[] = {
    PWM_DT_SPEC_GET_BY_NAME(_IJ, out1),  /* OUT1 */
    PWM_DT_SPEC_GET_BY_NAME(_IJ, out2),  /* OUT2 */
    PWM_DT_SPEC_GET_BY_NAME(_IJ, out3),  /* OUT3 */
    PWM_DT_SPEC_GET_BY_NAME(_IJ, out4),  /* OUT4 */
    PWM_DT_SPEC_GET_BY_NAME(_IJ, out5),  /* OUT5 */
    PWM_DT_SPEC_GET_BY_NAME(_IJ, out6),  /* OUT6 */
};

#if defined(CONFIG_SOC_SERIES_RP2040)
/* RP2040: Zephyr has no runtime pinmux API; set IO_BANK0 GPIO_CTRL.FUNCSEL
 * directly to switch between SIO (GPIO) and PWM functions. */
#define _RP2040_IO_BANK0 0x40014000UL
#define _IJ_FUNC_PWM     4U
#define _IJ_FUNC_SIO     5U

static void _ij_gpio_func(uint32_t pin, uint32_t func)
{
    volatile uint32_t *ctrl =
        (volatile uint32_t *)(_RP2040_IO_BANK0 + pin * 8U + 4U);
    *ctrl = (*ctrl & ~0x1FU) | (func & 0x1FU);
}
#endif /* CONFIG_SOC_SERIES_RP2040 */

void IJB_pwm(int port, int plen, int len)
{
    if (port < 1 || port > 6) return;
    if (plen < 0)        plen = 0;
    if (plen > PLEN_MAX) plen = PLEN_MAX;
    if (len  <= 0)       len  = PLEN_MAX;  /* 2000 × 0.01 ms = 20 ms */

    uint32_t period_ns = (uint32_t)len  * 10000U;
    uint32_t pulse_ns  = (uint32_t)plen * 10000U;
    if (pulse_ns > period_ns) pulse_ns = period_ns;

    const struct pwm_dt_spec *s = &_pwm_out[port - 1];
    if (!device_is_ready(s->dev)) return;
    pwm_set(s->dev, s->channel, period_ns, pulse_ns, s->flags);

#if defined(CONFIG_SOC_SERIES_RP2040)
    /* Switch GPIO pin mux to PWM function so the signal appears on the pin. */
    _ij_gpio_func((uint32_t)_out_spec[port - 1].pin, _IJ_FUNC_PWM);
#endif
}

S_INLINE void pwm_off(int port)
{
    if (port < 1 || port > 6) return;
    const struct pwm_dt_spec *s = &_pwm_out[port - 1];
    if (!device_is_ready(s->dev)) return;
    pwm_set(s->dev, s->channel, 10000000U, 0U, s->flags);

#if defined(CONFIG_SOC_SERIES_RP2040)
    /* Restore SIO function so gpio_pin_configure_dt() takes effect. */
    _ij_gpio_func((uint32_t)_out_spec[port - 1].pin, _IJ_FUNC_SIO);
    IJB_out(port, 0);
#endif
}

#else /* no pwms in overlay → stub */

void IJB_pwm(int port, int plen, int len) { (void)port; (void)plen; (void)len; }
S_INLINE void pwm_off(int port) { (void)port; }

#endif /* PWM OUT */

// --- I2C ---
//
// Device selected via: chosen { ij-i2c = <&i2c0>; } (or &lpi2c0, etc.)
// No board-specific label referenced here.
//
// IJB_i2c(writemode, param):
//   param[0] = 7-bit I2C address
//   param[1] = BASIC virtual addr of buf1 (always written first)
//   param[2] = len of buf1
//   param[3] = BASIC virtual addr of buf2
//   param[4] = len of buf2
//   writemode==0: write buf1, write buf2  (pure write)
//   writemode==1: write buf1, read  buf2  (register read)
//   Returns 0 on success, 1 on error.

#if DT_HAS_CHOSEN(ij_i2c)
#  define _IJ_I2C_DEV DEVICE_DT_GET(DT_CHOSEN(ij_i2c))
#  define _IJ_I2C_ENABLED 1
#else
#  define _IJ_I2C_ENABLED 0
#endif

int i2c0_init(void) {
#if _IJ_I2C_ENABLED
    const struct device *dev = _IJ_I2C_DEV;
    return device_is_ready(dev) ? 0 : 1;
#else
    return 1;
#endif
}

S_INLINE int IJB_i2c(uint8 writemode, uint16 *param) {
#if _IJ_I2C_ENABLED
    const struct device *dev = _IJ_I2C_DEV;
    if (!device_is_ready(dev)) return 1;

    uint8_t addr = (uint8_t)param[0];
    uint8_t *buf1 = (uint8_t *)(RAM_AREA + param[1] - OFFSET_RAMROM);
    int len1 = param[2];
    uint8_t *buf2 = (uint8_t *)(RAM_AREA + param[3] - OFFSET_RAMROM);
    int len2 = param[4];

    int r;
    if (writemode) {
        // read mode: write register address (buf1), read data (buf2)
        r = i2c_write_read(dev, addr, buf1, (uint32_t)len1,
                           buf2, (uint32_t)len2);
    } else {
        // write mode: write buf1, then write buf2
        r = i2c_write(dev, buf1, (uint32_t)len1, addr);
        if (r == 0 && len2 > 0) {
            r = i2c_write(dev, buf2, (uint32_t)len2, addr);
        }
    }
    return r == 0 ? 0 : 1;
#else
    (void)writemode; (void)param;
    return 1;
#endif
}

#endif // __ICHIGOJAM_IO_H__
