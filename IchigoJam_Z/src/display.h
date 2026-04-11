// IchigoJam_Z - display HAL (serial terminal, same as console port)

#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <zephyr/kernel.h>

INLINE void video_on(void) {
    SCREEN_W = 32;
    SCREEN_H = 24;
}

INLINE void video_off(int clkdiv) {
}

INLINE int video_active(void) {
    return 0;
}

INLINE void IJB_lcd(uint mode) {
}

// 1 tick = 1/60 sec ≈ 16.67ms
// video_waitSync(70) at boot gives ~1.17s for USB CDC enumeration
INLINE void video_waitSync(uint n) {
    if (n > 0) {
        k_sleep(K_MSEC((uint32_t)n * 1000 / 60));
    }
}

#endif // __DISPLAY_H__
