// IchigoJam_Z - sound HAL for Zephyr (stub for M0; PWM/PSG added in M3)

#ifndef __SOUND_H__
#define __SOUND_H__

INLINE void sound_init(void) {
}

void sound_on(void) {
}

void sound_off(void) {
}

S_INLINE void sound_tick(void) {
}

void sound_switch(int on) {
}

// set_tone() is called by timer_thread (M3). Stub for M0.
S_INLINE void set_tone(int freq) {
}

#endif // __SOUND_H__
