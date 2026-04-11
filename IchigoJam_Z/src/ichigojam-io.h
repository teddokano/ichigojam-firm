// IchigoJam_Z - GPIO / I2C / PWM HAL for Zephyr
// M0: stub implementation. GPIO/I2C wired in M2.

#ifndef __ICHIGOJAM_IO_H__
#define __ICHIGOJAM_IO_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>

// --- GPIO stubs (M2) ---

int i2c0_init(void) {
    return 0;
}

void io_init(void) {
}

S_INLINE int analog_get(int ch) {
    return 0;
}

#define ANA_THRESHOLD (1024 / 4)

S_INLINE int io_get(void) {
    return 0;
}

S_INLINE void io_set(int n) {
}

S_INLINE void IJB_led(int st) {
}

int IJB_in(void) {
    return io_get();
}

void IJB_out(int port, int st) {
}

int IJB_btn(int n) {
    return 0;
}

S_INLINE int IJB_ana(int n) {
    return 0;
}

S_INLINE void IJB_clo(void) {
    io_init();
}

// --- PWM stub ---

void IJB_pwm(int port, int plen, int len) {
}

S_INLINE void pwm_off(int port) {
}

// --- I2C stub ---

S_INLINE int IJB_i2c(uint8 writemode, uint16* param) {
    return 1; // io error (not implemented yet)
}

#endif // __ICHIGOJAM_IO_H__
