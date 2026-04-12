// IchigoJam_Z - sound HAL for Zephyr (PWM/PSG, M3)
//
// Timer thread fires at 60 Hz (16666 µs), calls psg_tick() to advance
// the MML/BEEP state machine, then drives the PWM output via set_tone().
//
// Board mapping:
//   rpi_pico       : &pwm  slice 2, channel A → GPIO20  (Zephyr ch 4)
//   frdm_mcxa153   : &flexpwm0_pwm0  sub-module 0, channel A → P3_6 (ch 0)

#ifndef __SOUND_H__
#define __SOUND_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>

// Board-specific PWM selection
//   rpi_pico    : &pwm           slice 2 chA → GPIO20
//   frdm_mcxa153: &ctimer0       CT0_MAT0    → D13 / P2_12  (sound moved off FlexPWM)
//   fallback    : &flexpwm0_pwm0 sm0 chA     → P3_6
#if DT_NODE_EXISTS(DT_NODELABEL(pwm))
#  define SOUND_PWM_NODE DT_NODELABEL(pwm)
#  define SOUND_PWM_CHAN 4U   /* rpi_pico: slice 2, channel A → GPIO20 */
#elif DT_NODE_EXISTS(DT_NODELABEL(ctimer0)) && \
      DT_NODE_HAS_COMPAT(DT_NODELABEL(ctimer0), nxp_ctimer_pwm)
#  define SOUND_PWM_NODE DT_NODELABEL(ctimer0)
#  define SOUND_PWM_CHAN 0U   /* frdm_mcxa153: CT0_MAT0 → D13 (P2_12) */
#elif DT_NODE_EXISTS(DT_NODELABEL(flexpwm0_pwm0))
#  define SOUND_PWM_NODE DT_NODELABEL(flexpwm0_pwm0)
#  define SOUND_PWM_CHAN 0U   /* fallback: sub-module 0, channel A → P3_6 */
#endif

#ifdef SOUND_PWM_NODE
static const struct device *_pwm_dev;
static bool _pwm_ready;
#endif

static bool _sound_on;

// set_tone: drive PWM at freq Hz (0 = silence). Only updates hardware when
// the "effective output" (freq × on/off) actually changes.
S_INLINE void set_tone(int freq)
{
    static int _prev_out_freq = -1;

    int out_freq = (_sound_on && freq > 0) ? freq : 0;

    if (out_freq == _prev_out_freq) {
        return;
    }
    _prev_out_freq = out_freq;

#ifdef SOUND_PWM_NODE
    if (!_pwm_ready) {
        return;
    }
    if (out_freq == 0) {
        // 0% duty cycle at 100 Hz = constant LOW = silence
        pwm_set(_pwm_dev, SOUND_PWM_CHAN, 10000000U, 0U, PWM_POLARITY_NORMAL);
    } else {
        uint32_t period_ns = 1000000000U / (uint32_t)out_freq;
        pwm_set(_pwm_dev, SOUND_PWM_CHAN, period_ns, period_ns / 2U,
                PWM_POLARITY_NORMAL);
    }
#endif
}

// Timer thread: 60 Hz (16666 µs), advances PSG state then updates PWM
#define SOUND_TIMER_STACK_SIZE 512
#define SOUND_TIMER_PRIORITY   K_PRIO_COOP(1)

K_THREAD_STACK_DEFINE(_sound_timer_stack, SOUND_TIMER_STACK_SIZE);
static struct k_thread _sound_timer_thread;

static void _sound_timer_fn(void *a, void *b, void *c)
{
    while (1) {
        k_sleep(K_USEC(16666));
        psg_tick();
        uint16_t tone = _g.psgtone;
        // Frequency formula from IchigoJam_P: freq = 60 * 261 / 2 / tone
        int freq = tone ? (7830 / (int)tone) : 0;
        set_tone(freq);
    }
}

INLINE void sound_init(void) {
    _g.psgratio = 1;
#ifdef SOUND_PWM_NODE
    _pwm_dev   = DEVICE_DT_GET(SOUND_PWM_NODE);
    _pwm_ready = device_is_ready(_pwm_dev);
#endif
    _sound_on = false;

    k_thread_create(&_sound_timer_thread,
                    _sound_timer_stack,
                    K_THREAD_STACK_SIZEOF(_sound_timer_stack),
                    _sound_timer_fn, NULL, NULL, NULL,
                    SOUND_TIMER_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&_sound_timer_thread, "psg_timer");
}

void sound_on(void) {
    _sound_on = true;
}

void sound_off(void) {
    _sound_on = false;
    set_tone(0);
}

static void sound_switch(int on) {
    if (on) {
        sound_on();
    } else {
        sound_off();
    }
}

// sound_tick: used in PSG_TRUE_TONE path only; no-op for TONE12
S_INLINE void sound_tick(void) {
}

#endif // __SOUND_H__
