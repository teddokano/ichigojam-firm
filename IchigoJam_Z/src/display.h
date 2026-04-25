// IchigoJam_Z - CVBS video output (M6)
//
// Circuit (Raspberry Pi Pico):
//   GPIO16 (SYNC)  ── 470Ω ──┬── CVBS出力 (75Ω終端テレビへ)
//   GPIO17 (VIDEO) ── 100Ω ──┘
//
// 信号レベル:
//   SYNC=L, VIDEO=L → 0V       (同期チップ)
//   SYNC=H, VIDEO=L → ~0.38V   (ブランク/黒)
//   SYNC=H, VIDEO=H → ~0.97V   (白)
//
// 実装方針: Zephyr counter API で 64µs 周期のライン ISR を生成。
//   1ライン = 64µs (NTSC 63.5µs に近似、60fps でテレビ同期可)
//   261ライン/フレーム: vsync(3) + vblank(19) + active(192) + vblank(47)
//
// アクティブライン ISR 内での映像出力:
//   32 キャラクタ列 × k_busy_wait(1) = 32µs のファットピクセル出力。
//   横幅は理想の 61% 程度だが文字の判読は可能。
//   BASIC インタープリタの CPU 占有率: ~45% (ブランク期間が主体)。
//
// RP2040 SIO レジスタを SYNC/VIDEO ピンの高速トグルに使用:
//   Zephyr GPIO API (~200ns/call) は 1µs ファットピクセルには十分だが、
//   ISR 先頭での SYNC アサートは SIO 直叩きで遅延を最小化する。
//   SIO は Zephyr GPIO driver が内部で使うレジスタと同じであり安全。
//
// 非 RP2040 ボード: #if CONFIG_SOC_SERIES_RP2040 で全ブロックをガード。
//   frdm_mcxa153 / frdm_mcxc444 では video_on() は画面サイズ設定のみ。

#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <zephyr/kernel.h>

#if defined(CONFIG_SOC_SERIES_RP2040)
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/counter.h>

/* ── NTSC タイミング定数 ────────────────────────────────────────── */
#define _CVBS_LINES      261    /* 総ライン数 */
#define _CVBS_ACT_FIRST  22     /* アクティブ開始ライン (vsync3 + vblank19) */
#define _CVBS_ACT_LINES  192    /* アクティブライン数 (24行 × 8px) */
#define _CVBS_HTOTAL_US  64     /* 1ライン周期 µs */
#define _CVBS_HSYNC_US   5      /* 水平同期パルス幅 µs */
#define _CVBS_BACK_US    5      /* バックポーチ µs */
#define _CVBS_VSYNC_US   32     /* 垂直同期ロングパルス µs */

/* ── GPIO / counter バインディング ─────────────────────────────── */
static const struct gpio_dt_spec _cvbs_sync =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ij_cvbs_sync_gpios);
static const struct gpio_dt_spec _cvbs_video =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ij_cvbs_video_gpios);
static const struct device *_cvbs_ctr;

/* ── RP2040 SIO 直接アクセス ────────────────────────────────────
 * SYNC ISR 先頭での遅延を最小化するため SIO SET/CLR レジスタを直叩き。
 * Zephyr GPIO API の内部でも同じ SIO を使用しており整合性に問題ない。
 * 対象: SIO_BASE=0xD0000000, GPIO_OUT_SET=+0x14, GPIO_OUT_CLR=+0x18 */
#define _SIO_SET (*(volatile uint32_t *)0xD0000014U)
#define _SIO_CLR (*(volatile uint32_t *)0xD0000018U)
static uint32_t _sync_mask;    /* (1U << 16) for GPIO16 */
static uint32_t _video_mask;   /* (1U << 17) for GPIO17 */

/* ── 状態変数 ───────────────────────────────────────────────────── */
static volatile uint16_t _cvbs_line;
static struct counter_alarm_cfg _cvbs_alarm;
static bool _cvbs_on;

/* ── ライン ISR ─────────────────────────────────────────────────
 * counter alarm callback。ISR 先頭で次ラインのアラームを再スケジュール
 * してから SYNC / 映像出力を行う。相対モード(64µs)で再登録するため
 * 実際の発火は約 ticks+1+64µs (1µs は ISR エントリ遅延)。
 * TV はこの程度の H-sync ジッタを許容する。                          */
static void _cvbs_line_cb(const struct device *dev, uint8_t chan,
                          uint32_t ticks, void *user_data)
{
    ARG_UNUSED(ticks);
    ARG_UNUSED(user_data);

    /* 次ラインを今すぐ予約 (処理より前に登録して遅延を最小化) */
    _cvbs_alarm.ticks = _CVBS_HTOTAL_US;
    counter_set_channel_alarm(dev, chan, &_cvbs_alarm);

    uint16_t ln = _cvbs_line;
    _cvbs_line = (ln + 1u < _CVBS_LINES) ? (uint16_t)(ln + 1u) : 0u;

    /* フレーム/ライン カウンタ更新 (TICK() / WAIT コマンドが参照) */
    _g.linecnt++;
    if (ln == 0u) {
        frames++;
    }

    if (ln < 3u) {
        /* ── Vsync ライン: ロングパルス ─────────────────────────── */
        _SIO_CLR = _sync_mask | _video_mask;   /* SYNC=L, VIDEO=L */
        k_busy_wait(_CVBS_VSYNC_US);
        _SIO_SET = _sync_mask;                 /* SYNC=H */
        return;
    }

    /* ── 通常ライン: 水平同期 ───────────────────────────────────── */
    _SIO_CLR = _sync_mask | _video_mask;       /* SYNC=L (hsync) */
    k_busy_wait(_CVBS_HSYNC_US);
    _SIO_SET = _sync_mask;                     /* SYNC=H */
    k_busy_wait(_CVBS_BACK_US);               /* バックポーチ */

    if (ln >= _CVBS_ACT_FIRST && ln < (uint16_t)(_CVBS_ACT_FIRST + _CVBS_ACT_LINES)) {
        /* ── アクティブライン: 映像出力 ─────────────────────────── */
        int vln = (int)ln - _CVBS_ACT_FIRST;
        int cy  = vln >> 3;   /* キャラクタ行 0..23 */
        int sr  = vln & 7;    /* キャラクタ内スキャン行 0..7 */

        const uint8_t *vrow = vram + cy * 32;

        for (int col = 0; col < 32; col++) {
            uint8_t ch  = vrow[col];
            /* PCG (コード 0-31) は screen_pcg、それ以外は CHAR_PATTERN */
            uint8_t pat = (ch < SIZE_PCG)
                ? screen_pcg[ch * 8 + sr]
                : CHAR_PATTERN[ch * 8 + sr];

            /* ファットピクセル: スキャン行に 1 ビットでも立っていれば白。
             * 1 列 = k_busy_wait(1) = 1µs。32 列で約 32µs の映像ウィンドウ。
             * TV 上では横幅が理想の ~61% になるが文字は判読可能。         */
            if (pat != 0u) {
                _SIO_SET = _video_mask;        /* VIDEO=H (白) */
            } else {
                _SIO_CLR = _video_mask;        /* VIDEO=L (黒) */
            }
            k_busy_wait(1u);
        }
        _SIO_CLR = _video_mask;                /* VIDEO=L (フロントポーチ) */
    }
}

/* ── 公開 API ────────────────────────────────────────────────────── */

INLINE void video_on(void) {
    SCREEN_W = 32;
    SCREEN_H = 24;
    if (_cvbs_on) { return; }

    gpio_pin_configure_dt(&_cvbs_sync,  GPIO_OUTPUT_LOW);
    gpio_pin_configure_dt(&_cvbs_video, GPIO_OUTPUT_LOW);

    _sync_mask  = (1U << (uint32_t)_cvbs_sync.pin);
    _video_mask = (1U << (uint32_t)_cvbs_video.pin);

    _cvbs_ctr = DEVICE_DT_GET(DT_CHOSEN(ij_cvbs_timer));
    _cvbs_line = 0u;

    _cvbs_alarm.callback  = _cvbs_line_cb;
    _cvbs_alarm.ticks     = _CVBS_HTOTAL_US;
    _cvbs_alarm.flags     = 0u;
    _cvbs_alarm.user_data = NULL;

    counter_start(_cvbs_ctr);
    counter_set_channel_alarm(_cvbs_ctr, 0, &_cvbs_alarm);
    _cvbs_on = true;
}

INLINE void video_off(int clkdiv) {
    ARG_UNUSED(clkdiv);
    if (!_cvbs_on) { return; }
    /* アラームをキャンセルするだけ。counter_stop() はタイマをリセットする
     * ため呼ばない (他の用途に影響する可能性がある)。                    */
    counter_cancel_channel_alarm(_cvbs_ctr, 0);
    _SIO_CLR = _sync_mask | _video_mask;
    _cvbs_on = false;
}

INLINE int video_active(void) {
    return _cvbs_on ? 1 : 0;
}

INLINE void IJB_lcd(uint mode) {
    ARG_UNUSED(mode);
    /* SWITCH コマンド: CVBS では未使用 */
}

/* video_waitSync(n): n フレーム待機。
 * CVBS 動作中は frames カウンタをポーリング、停止中は k_sleep で代替。
 * CLAUDE.md の制約: M6 以降も最低 16ms の yield を保証すること。      */
INLINE void video_waitSync(uint n) {
    if (n == 0u) { return; }
    if (_cvbs_on) {
        uint16_t target = (uint16_t)((uint16_t)frames + (uint16_t)n);
        while ((int16_t)(target - frames) > 0) {
            k_sleep(K_MSEC(1));   /* yield しつつポーリング */
        }
    } else {
        k_sleep(K_MSEC((uint32_t)n * 1000u / 60u));
    }
}

#else /* !CONFIG_SOC_SERIES_RP2040 */

/* ── 非 RP2040 ボード: CVBS なし ────────────────────────────────
 * frdm_mcxa153 / frdm_mcxc444 では CVBS 出力を実装しない。
 * video_waitSync は k_sleep で 1/60s ずつ yield する。               */

INLINE void video_on(void) {
    SCREEN_W = 32;
    SCREEN_H = 24;
}

INLINE void video_off(int clkdiv) {
    ARG_UNUSED(clkdiv);
}

INLINE int video_active(void) {
    return 0;
}

INLINE void IJB_lcd(uint mode) {
    ARG_UNUSED(mode);
}

/* 1 tick = 1/60 sec ≈ 16.67ms
 * video_waitSync(70) at boot gives ~1.17s for USB CDC enumeration */
INLINE void video_waitSync(uint n) {
    if (n > 0u) {
        k_sleep(K_MSEC((uint32_t)n * 1000u / 60u));
    }
}

#endif /* CONFIG_SOC_SERIES_RP2040 */

#endif /* __DISPLAY_H__ */
