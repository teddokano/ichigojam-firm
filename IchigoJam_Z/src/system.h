// IchigoJam_Z - system HAL for Zephyr

#ifndef __SYSTEM_H__
#define __SYSTEM_H__

#include <zephyr/kernel.h>

void system_init(void) {
}

int getSleepFlag(void) {
    return 0;
}

void enterDeepSleep(int waitsec) {
    k_sleep(K_SECONDS(waitsec));
}

INLINE void deepPowerDown(void) {
}

INLINE void IJB_sleep(void) {
}

S_INLINE void IJB_reset(void) {
    // Zephyr does not provide a portable reset API for RP2040 in all versions.
    // Use NVIC reset if available; fall back to infinite loop.
    NVIC_SystemReset();
}

// forward declaration: defined in keyboard.h which is included after system.h
int stopExecute(void);

// IJB_wait: wait n ticks (1 tick = 1/60 sec), poll for ESC each 16ms
int IJB_wait(int n, int active) {
    int ticks = n;
    while (ticks > 0) {
        k_sleep(K_MSEC(16));
        ticks--;
        if (stopExecute()) {
            return 1;
        }
    }
    return 0;
}

#endif // __SYSTEM_H__
