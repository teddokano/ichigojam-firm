// IchigoJam_Z — display HAL (Zephyr)
//
// FRDM-MCXA153 : NTSC composite video via CTimer1 + LPSPI0 + sync GPIO
// Other boards  : serial-terminal stub (unchanged behaviour)
//
// ── Signal circuit ───────────────────────────────────────────────────────────
//
//   GPIO_SYNC (P1_3) ── 560 Ω ──┐
//                                ├── CVBS out ── [75 Ω] ── GND
//   LPSPI0_MOSI(P1_0) ── 270 Ω ──┘
//
//   Composite levels (3.3 V supply):
//     SYNC=L, MOSI=L  →  0.00 V  sync tip
//     SYNC=H, MOSI=L  →  0.39 V  black / blank
//     SYNC=H, MOSI=H  →  0.96 V  white
//
// ── NTSC timing (96 MHz system clock) ────────────────────────────────────────
//
//   Line period      : 6104 cy = 63.58 µs  (261 lines × 60.2 Hz)
//   H-sync LOW       :  451 cy =  4.70 µs
//   Back porch       :  559 cy =  5.82 µs
//   Active video     : 5050 cy = 52.60 µs  → 32 bytes via LPSPI0 @ 6.00 MHz
//   Front porch      :   44 cy =  0.46 µs
//
//   LPSPI0 clock source: FRO12M (12 MHz) via board.c, max SPI SCK = 12/2 = 6 MHz
//   At 6 MHz: 32 bytes = 256 bits → 42.67 µs (fits in 52.60 µs active window)
//
//   H-sync busy-wait uses CTimer1 TC register directly (counts at 96 MHz).
//   TC resets to 0 on MR0 match; ISR waits until TC >= 451 before SYNC HIGH.
//
//   Frame (261 lines):
//     Lines   0– 2 :   3 V-sync   (SYNC stays LOW full line; ISR returns fast)
//     Lines   3–21 :  19 blank
//     Lines  22–213: 192 active   (24 char rows × 8 px)
//     Lines 214–260:  47 blank
//
// ── Hardware resources ────────────────────────────────────────────────────────
//
//   CTimer1 (IRQ 40, base 0x40005000): line timing at 15.734 kHz
//   LPSPI0  (DT_NODELABEL(lpspi0))  : SPI pixel shift-out, 6.00 MHz
//   GPIO1 pin 3 (P1_3)              : sync pulse output (active-low in DTS)
//
// ── Threads ──────────────────────────────────────────────────────────────────
//
//   CTimer1 ISR           : fires each line, drives sync GPIO,
//                           signals video_thread via semaphore
//   video_thread COOP(0)  : sends current line buffer via SPI,
//                           renders next line from VRAM + CHAR_PATTERN

#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <zephyr/kernel.h>

// ═══════════════════════════════════════════════════════════════════════════════
// NTSC composite section — compiled only for boards with ctimer1 enabled
// ═══════════════════════════════════════════════════════════════════════════════
#if DT_NODE_HAS_STATUS(DT_NODELABEL(ctimer1), okay)

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <fsl_clock.h>   // NXP HAL: CLOCK_EnableClock (available via hal_nxp)

// ── NTSC timing constants at 96 MHz ──────────────────────────────────────────
#define _NTSC_LINE_CY       6104U   // total line period
#define _NTSC_SYNC_CY        451U   // H-sync LOW duration
#define _NTSC_TOTAL_LINES    261
#define _NTSC_VSYNC_LINES      3    // simplified V-sync: full-line LOW × 3
#define _NTSC_ACTIVE_FIRST    22    // first active scanline
#define _NTSC_ACTIVE_H       192    // active height (24 rows × 8 px)

// ── CTimer1 raw registers (NXP LPC-style CTimer at 0x40005000) ───────────────
#define _CT1_BASE  0x40005000U
#define _CT1_IR    (*(volatile uint32_t *)(_CT1_BASE + 0x00U))  // interrupt
#define _CT1_TCR   (*(volatile uint32_t *)(_CT1_BASE + 0x04U))  // timer ctrl
#define _CT1_TC    (*(volatile uint32_t *)(_CT1_BASE + 0x08U))  // timer counter
#define _CT1_PR    (*(volatile uint32_t *)(_CT1_BASE + 0x0CU))  // prescale
#define _CT1_MCR   (*(volatile uint32_t *)(_CT1_BASE + 0x14U))  // match ctrl
#define _CT1_MR0   (*(volatile uint32_t *)(_CT1_BASE + 0x18U))  // match reg 0
#define _CT1_IRQ   40  // from SoC DTS: interrupts = <40 0>

// H-sync busy-wait uses _CT1_TC directly (counts at 96 MHz, resets to 0 on
// MR0 match — the same event that triggers the ISR).  No DWT needed.

// ── SYNC GPIO (P1_3 = GPIO1 pin 3, GPIO_ACTIVE_LOW in overlay) ───────────────
// SYNC LOW  → sync tip  (gpio_pin_set_dt value = 1 with ACTIVE_LOW)
// SYNC HIGH → blank/vid (gpio_pin_set_dt value = 0)
// In the ISR we bypass the dt layer for speed: use port_clear/set_bits_raw
static const struct gpio_dt_spec _sync =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ij_sync_gpios);

// Macros for ISR-safe raw GPIO (avoids active-low overhead in hot path)
#define _SYNC_LOW()   gpio_port_clear_bits_raw(_sync.port, BIT(_sync.pin))
#define _SYNC_HIGH()  gpio_port_set_bits_raw  (_sync.port, BIT(_sync.pin))
// Note: ij_sync_gpios uses GPIO_ACTIVE_LOW so clear→pin LOW (sync tip)

// ── LPSPI0 SPI device ─────────────────────────────────────────────────────────
// Configured TX-only: SDO=P1_0, SCK=P1_1 (SCK not wired externally).
// LPSPI0 is clocked from FRO12M (12 MHz) via board.c.
// Max achievable SCK = 12 MHz / 2 = 6 MHz → 32 bytes in 42.67 µs < 52.60 µs window.
static const struct device *_spi_dev;
static const struct spi_config _spi_cfg = {
    .frequency = 6000000U,   // driver picks 12/2 = 6 MHz (max from FRO12M source)
    .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER,
    .slave     = 0,
    /* .cs not set → zero-initialized → no chip-select driven */
};

// ── Line pixel buffers (double-buffered, 256 px × 1 bit = 32 bytes) ──────────
static uint8_t _lbuf[2][32];   // [0]=buf A, [1]=buf B
static int _lbuf_disp;         // video_thread sends this buffer
static int _lbuf_rend;         // video_thread renders into this buffer

// ── Frame / line state ────────────────────────────────────────────────────────
static volatile int      _vline;    // current scanline [0..260], written by ISR
static volatile uint32_t _vframe;   // frame counter, written by ISR

// ── ISR → video_thread semaphore ─────────────────────────────────────────────
K_SEM_DEFINE(_vsem, 0, 1);

// ── VRAM rendering ────────────────────────────────────────────────────────────
// CHAR_PATTERN[ch*8 + row]: 1 byte = 8 horizontal pixels, MSB = leftmost pixel
// RAM_VRAM: base of VRAM character buffer (macro from IchigoJam BASIC headers)
extern uint8 CHAR_PATTERN[];

static void _render_line(int scanline, uint8_t *buf)
{
    int rel = scanline - _NTSC_ACTIVE_FIRST;
    if (rel < 0 || rel >= _NTSC_ACTIVE_H) {
        memset(buf, 0x00, 32);
        return;
    }
    int char_row = rel >> 3;                         // /8
    int font_row = rel &  7;                         // %8
    const uint8_t *vrow = (const uint8_t *)RAM_VRAM + char_row * 32;
    for (int col = 0; col < 32; col++) {
        buf[col] = CHAR_PATTERN[(uint8_t)vrow[col] * 8 + font_row];
    }
}

// ── CTimer1 ISR ───────────────────────────────────────────────────────────────
// TC resets to 0 on every MR0 match (the same event that asserts this IRQ).
// Worst-case ISR runtime: busy-wait until TC ≥ 451 ≈ 4.7 µs (H-sync pulse).
// V-sync lines just drive SYNC LOW and return immediately (no busy-wait).
static void _ctimer1_isr(const void *arg)
{
    ARG_UNUSED(arg);
    _CT1_IR = 1U;   // clear MR0 match flag

    int line = _vline;

    if (line == 0) {
        _vframe++;
    }

    if (line < _NTSC_VSYNC_LINES) {
        // V-sync: hold SYNC LOW for the entire line.
        // Three consecutive full-line LOWs ≈ 191 µs → satisfies NTSC V-sync.
        _SYNC_LOW();
    } else {
        // Normal H-sync: drive LOW, busy-wait on TC until 451 cy (4.70 µs),
        // then HIGH.  TC started from 0 when MR0 match fired this ISR.
        _SYNC_LOW();
        while (_CT1_TC < _NTSC_SYNC_CY);
        _SYNC_HIGH();
    }

    _vline = (line + 1 < _NTSC_TOTAL_LINES) ? line + 1 : 0;
    k_sem_give(&_vsem);
}

// ── Video thread ──────────────────────────────────────────────────────────────
#define _VID_STACK_SZ 2048
#define _VID_PRIO     K_PRIO_COOP(0)   // highest: runs immediately on sem_give

K_THREAD_STACK_DEFINE(_vid_stack, _VID_STACK_SZ);
static struct k_thread _vid_thread;

static void _vid_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    struct spi_buf     tx    = { .buf = NULL, .len = 32 };
    struct spi_buf_set txset = { .buffers = &tx, .count = 1 };

    while (1) {
        k_sem_take(&_vsem, K_FOREVER);

        // Compute which scanline the ISR just finished
        int line = _vline - 1;
        if (line < 0) line += _NTSC_TOTAL_LINES;

        // Send the pre-rendered buffer for this scanline
        int rel = line - _NTSC_ACTIVE_FIRST;
        if (rel >= 0 && rel < _NTSC_ACTIVE_H) {
            tx.buf = _lbuf[_lbuf_disp];
            spi_write(_spi_dev, &_spi_cfg, &txset);
        }

        // Render the *next* scanline into the render buffer
        _render_line(line + 1, _lbuf[_lbuf_rend]);

        // Swap buffers: next ISR cycle will display what we just rendered
        int tmp    = _lbuf_disp;
        _lbuf_disp = _lbuf_rend;
        _lbuf_rend = tmp;
    }
}

// ── CTimer1 hardware init ─────────────────────────────────────────────────────
static void _ctimer1_hw_init(void)
{
    // Enable CTimer1 clock gate (clock source FRO_HF→CTimer1 set in board.c)
    CLOCK_EnableClock(kCLOCK_GateCTIMER1);

    // Reset then configure:
    //   prescale=0 (96 MHz tick), MR0 = line period - 1,
    //   MCR[1:0] = 0b11 → interrupt + reset on MR0 match
    _CT1_TCR = 2U;                      // hold in reset
    _CT1_PR  = 0U;                      // no prescale
    _CT1_MR0 = _NTSC_LINE_CY - 1U;
    _CT1_MCR = 0x03U;                   // MR0: interrupt + reset
    _CT1_IR  = 0x0FU;                   // clear any stale flags

    // Register ISR and enable IRQ (priority 1, below highest kernel IRQ)
    IRQ_CONNECT(_CT1_IRQ, 1, _ctimer1_isr, NULL, 0);
    irq_enable(_CT1_IRQ);

    // Start counting
    _CT1_TCR = 1U;
}

// ── Public HAL interface ──────────────────────────────────────────────────────

INLINE void video_on(void)
{
    SCREEN_W = 32;
    SCREEN_H = 24;

    // Init SPI device reference
    _spi_dev = DEVICE_DT_GET(DT_NODELABEL(lpspi0));

    // Init sync GPIO as output, initially HIGH (no sync)
    gpio_pin_configure_dt(&_sync, GPIO_OUTPUT_INACTIVE);

    // Pre-render line 0 into buffer 0; buffer 1 starts blank
    _render_line(0, _lbuf[0]);
    memset(_lbuf[1], 0x00, 32);
    _lbuf_disp = 0;
    _lbuf_rend = 1;
    _vline  = 0;
    _vframe = 0;

    // Start video thread (woken by CTimer1 ISR each line)
    k_thread_create(&_vid_thread, _vid_stack,
                    K_THREAD_STACK_SIZEOF(_vid_stack),
                    _vid_thread_fn, NULL, NULL, NULL,
                    _VID_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&_vid_thread, "video");

    // Start CTimer1 line sync
    _ctimer1_hw_init();
}

INLINE void video_off(int clkdiv)
{
    (void)clkdiv;
    irq_disable(_CT1_IRQ);
    _CT1_TCR = 2U;   // stop + reset CTimer1
}

INLINE int video_active(void) { return 1; }

INLINE void IJB_lcd(uint mode) { (void)mode; }

// video_waitSync(n): wait for n frame edges (1 frame ≈ 1/60 s)
INLINE void video_waitSync(uint n)
{
    if (n == 0) return;
    uint32_t target = _vframe + (uint32_t)n;
    while (_vframe < target) {
        k_sleep(K_MSEC(1));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Serial-terminal stub — all other boards
// ═══════════════════════════════════════════════════════════════════════════════
#else  /* !ctimer1 */

INLINE void video_on(void)       { SCREEN_W = 32; SCREEN_H = 24; }
INLINE void video_off(int c)     { (void)c; }
INLINE int  video_active(void)   { return 0; }
INLINE void IJB_lcd(uint mode)   { (void)mode; }

// 1 tick = 1/60 s; video_waitSync(70) at boot gives ~1.17 s for CDC enum
INLINE void video_waitSync(uint n)
{
    if (n > 0) {
        k_sleep(K_MSEC((uint32_t)n * 1000U / 60U));
    }
}

#endif /* ctimer1 */

#endif /* __DISPLAY_H__ */
