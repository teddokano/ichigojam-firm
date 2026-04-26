// IchigoJam_Z - sound HAL for Zephyr (PWM/PSG, M3)
//
// Timer thread fires at 60 Hz (16666 µs), calls psg_tick() to advance
// the MML/BEEP state machine, then drives the PWM output via set_tone().
//
// Board mapping is fully in the DT overlay (zephyr,user / ij-sound-pwms):
//   rpi_pico       : &pwm  ch 4  (GPIO20 = slice 2, channel A)
//   frdm_mcxa153   : &ctimer0 ch 0  (CT0_MAT0 → D13 / P2_12)
//
// No board-specific labels appear in this file.

#ifndef __SOUND_H__
#define __SOUND_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>

// Sound PWM spec from overlay:
//   zephyr,user { pwms = <...>; pwm-names = "sound", ...; }
// Accessed by name so board mapping stays entirely in the overlay.
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), pwms)
static const struct pwm_dt_spec _snd =
    PWM_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), sound);
#define _SOUND_PWM_ENABLED 1
#else
#define _SOUND_PWM_ENABLED 0
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

#if _SOUND_PWM_ENABLED
    if (!device_is_ready(_snd.dev)) {
        return;
    }
    if (out_freq == 0) {
        // 0% duty cycle at 100 Hz = constant LOW = silence
        pwm_set(_snd.dev, _snd.channel, 10000000U, 0U, _snd.flags);
    } else {
        uint32_t period_ns = 1000000000U / (uint32_t)out_freq;
        pwm_set(_snd.dev, _snd.channel, period_ns, period_ns / 2U, _snd.flags);
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
#if !defined(CONFIG_SOC_SERIES_RP2040)
        /* RP2040 では CVBS ライン ISR (_cvbs_line_cb) が
         * frames / _g.linecnt を更新するため、ここでは行わない。
         * 二重インクリメントすると TICK(0) が 2x 速になる。 */
        frames++;        // TICK(0) / video_waitSync frame counter
        _g.linecnt++;    // TICK(1) / WAIT -n line counter
#endif
        uint16_t tone = _g.psgtone;
        // Frequency formula from IchigoJam_P: freq = 60 * 261 / 2 / tone
        int freq = tone ? (7830 / (int)tone) : 0;
        set_tone(freq);
    }
}

INLINE void sound_init(void) {
    _g.psgratio = 1;
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
