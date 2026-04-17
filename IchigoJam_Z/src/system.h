// IchigoJam_Z - system HAL for Zephyr

#ifndef __SYSTEM_H__
#define __SYSTEM_H__

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

void system_init(void) {
    _g.screen_big         = 0;
    _g.screen_invert      = 0;
    _g.lastfile           = 0;
    _g.screen_insertmode  = 1;
    key_flg.insert        = 0;
    // key_flg is zero-initialised at startup; noresmode alias needs no explicit set
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
    sys_reboot(SYS_REBOOT_COLD);
}

// forward declaration: defined in keyboard.h which is included after system.h
int stopExecute(void);

// IJB_wait: wait n ticks (1 tick ≈ 1/60 s).
//   n > 0: sleep n×16 ms, polling ESC each tick
//   n < 0: wait |n| frames via _g.linecnt (incremented by sound timer at 60 Hz)
int IJB_wait(int n, int active) {
    if (n < 0) {
        _g.linecnt = 0;
        uint16_t target = (uint16_t)(-n);
        while (_g.linecnt < target) {
            k_sleep(K_MSEC(1));
            if (stopExecute()) return 1;
        }
        return 0;
    }
    int ticks = n;
    while (ticks > 0) {
        k_sleep(K_MSEC(16));
        ticks--;
        if (stopExecute()) return 1;
    }
    return 0;
}

#endif // __SYSTEM_H__
