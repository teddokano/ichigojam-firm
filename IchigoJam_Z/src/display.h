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
//   261ライン/フレーム: vsync(1) + vblank(20) + active(192) + vblank(48)
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
#define _CVBS_ACT_FIRST  21     /* アクティブ開始ライン (vsync1 + vblank20) */
#define _CVBS_ACT_LINES  192    /* アクティブライン数 (24行 × 8px) */
#define _CVBS_HTOTAL_US  64     /* 1ライン周期 µs */
#define _CVBS_HSYNC_US   4      /* 水平同期パルス幅 µs (IchigoJam_Q 実測 ~4µs) */
#define _CVBS_BACK_US    4      /* バックポーチ µs */
#define _CVBS_VSYNC_US   58     /* 垂直同期ロングパルス µs (58/64=91% duty — vsync 積分器確実トリガ) */

/* ── GPIO / counter バインディング ─────────────────────────────── */
/* static を付けない: INLINE が non-static inline に展開される場合に
 * "static variable used in non-static inline function" warning が出るため。
 * display.h は単一 TU にのみ include されるので多重定義の心配はない。  */
const struct gpio_dt_spec _cvbs_sync =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ij_cvbs_sync_gpios);
const struct gpio_dt_spec _cvbs_video =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ij_cvbs_video_gpios);
const struct device *_cvbs_ctr;

/* ── RP2040 SIO 直接アクセス ────────────────────────────────────
 * SYNC ISR 先頭での遅延を最小化するため SIO SET/CLR レジスタを直叩き。
 * Zephyr GPIO API の内部でも同じ SIO を使用しており整合性に問題ない。
 * 対象: SIO_BASE=0xD0000000, GPIO_OUT_SET=+0x14, GPIO_OUT_CLR=+0x18 */
#define _SIO_SET (*(volatile uint32_t *)0xD0000014U)
#define _SIO_CLR (*(volatile uint32_t *)0xD0000018U)
uint32_t _sync_mask;    /* (1U << 16) for GPIO16 */
uint32_t _video_mask;   /* (1U << 17) for GPIO17 */

/* ── 状態変数 ───────────────────────────────────────────────────── */
volatile uint16_t _cvbs_line;
struct counter_alarm_cfg _cvbs_alarm;
bool _cvbs_on;
/* 次ライン ISR の「本来の」絶対発火時刻 (µs, 1MHz カウンタ値)。
 * ISR が遅延した場合でも次のアラームを本来の時刻から計算することで
 * ジッタを1ライン内に閉じ込め、長期的な H-sync 周期を安定させる。 */
volatile uint32_t _cvbs_next_line_time;

/* ── ライン ISR ─────────────────────────────────────────────────
 * counter alarm callback。ISR 先頭で次ラインのアラームを再スケジュール
 * してから SYNC / 映像出力を行う。
 *
 * ジッタ補正: _cvbs_next_line_time (絶対時刻) を基準に次の alarm を
 * 設定することで、ISR 遅延を 1ライン内に閉じ込め H-sync 周期を安定化。
 * alarm0 IRQ 優先度 = 1 (overlay で設定) により SysTick (優先度 3) が
 * 本 ISR を割り込めず、ジッタの主因を根本から除去している。           */
void _cvbs_line_cb(const struct device *dev, uint8_t chan,
                   uint32_t ticks, void *user_data)
{
    ARG_UNUSED(user_data);

    /* ── 次ライン アラームをジッタ補正付きでスケジュール ────────────
     * ticks = このコールバックが呼ばれた時点の現在時刻 (µs)。
     * _cvbs_next_line_time は「本来の」絶対発火時刻を保持する。
     * ISR が遅延していても次は本来の時刻から 64µs 後に発火させることで
     * H-sync 周期のジッタを 1ライン内に吸収し長期安定性を保つ。        */
    _cvbs_next_line_time += _CVBS_HTOTAL_US;
    int32_t _delay = (int32_t)(_cvbs_next_line_time - ticks);
    if (_delay <= 0) {
        /* 大きな遅延 (複数ライン分) が発生した場合は次周期から再同期 */
        _cvbs_next_line_time = ticks + _CVBS_HTOTAL_US;
        _delay = _CVBS_HTOTAL_US;
    }
    _cvbs_alarm.ticks = (uint32_t)_delay;
    if (counter_set_channel_alarm(dev, chan, &_cvbs_alarm) == -ETIME) {
        /* アラーム設定時に "missed"（対象時刻が過去）と判定された場合、
         * driver が callback を NULL にして -ETIME を返すため、
         * そのままでは ISR チェーンが断絶する。
         * 次周期 (64µs後) から再同期して連鎖を維持する。       */
        _cvbs_next_line_time = ticks + _CVBS_HTOTAL_US;
        _cvbs_alarm.ticks   = _CVBS_HTOTAL_US;
        counter_set_channel_alarm(dev, chan, &_cvbs_alarm);
    }

    uint16_t ln = _cvbs_line;
    _cvbs_line = (ln + 1u < _CVBS_LINES) ? (uint16_t)(ln + 1u) : 0u;

    /* フレーム/ライン カウンタ更新 (TICK() / WAIT コマンドが参照) */
    _g.linecnt++;

    if (ln == 0u) {
        /* ── Vsync ライン: ロングパルス (1ライン) ───────────────── */
        /* vsync を複数ライン連続すると TV の同期セパレータが         */
        /* 複数回 vsync 検出し画面が乱れるため 1ライン のみ使用。    */
        frames++;
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

    /* 初期発火時刻を現在時刻 + 1周期に設定 */
    uint32_t _t0;
    counter_get_value(_cvbs_ctr, &_t0);
    _cvbs_next_line_time = _t0 + _CVBS_HTOTAL_US;

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
