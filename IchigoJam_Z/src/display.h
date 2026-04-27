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
//   261ライン/フレーム: vsync(12) + vblank(21) + active(192) + vblank(36)
//
// アクティブライン ISR 内での映像出力:
//   32 キャラクタ列 × 8ビット × ~125ns/ビット ≈ 1µs/キャラクタ = 32µs の映像ウィンドウ。
//   各ビットを個別に VIDEO=H/L でドライブして文字を判読可能に描画する。
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
#define _CVBS_ACT_FIRST  33     /* アクティブ開始ライン (vsync12 + vblank21): 9→21に増加で画像を下にシフト */
#define _CVBS_ACT_LINES  192    /* アクティブライン数 (24行 × 8px) */
#define _CVBS_HTOTAL_US  64     /* 1ライン周期 µs */
#define _CVBS_HSYNC_US   4      /* 水平同期パルス幅 µs (IchigoJam_Q 実測 ~4µs) */
#define _CVBS_BACK_US    8      /* バックポーチ µs (4µs では左寄りすぎるため 8µs に拡大) */
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

/* ── RP2040 タイマー直接待機 ────────────────────────────────────
 * k_busy_wait() は sys_clock_cycle_get_32() のスピンロック+SysTick読み取り
 * オーバーヘッド (~45 cycles ≈ 0.36µs/call) により k_busy_wait(1) が
 * 実質 ~2µs になり、32列ピクセル出力で ISR が 64µs を超過する。
 * TIMELR (0x4005400C) = 1MHz フリーランカウンタを直接読んで正確に待機。
 * counter_rpi_pico_timer が内部で使うレジスタと同じであり読み取り専用で安全。
 * ZephyrにAPIがないため直叩きを使う。                                   */
#define _CVBS_TIMER_RAW (*(volatile uint32_t *)0x4005400CU) /* TIMER: TIMELR (1MHz) */
#define _CVBS_WAIT_US(us) do { \
    uint32_t _t = _CVBS_TIMER_RAW + (uint32_t)(us); \
    while ((int32_t)(_CVBS_TIMER_RAW - _t) < 0) {} \
} while (0)

/* ── RP2040 ブランチレスピクセル出力 ──────────────────────────────
 * 引数 p: 現在のビットを MSB (bit 7) に持つ uint8_t。
 * 呼び出し元は `p = (uint8_t)(p << 1)` で次ビットをシフトする。
 *
 * SIO の仕様: SET/CLR レジスタへの書き込み値 0 は無操作 (no-op)。
 *   ビット=1 → _s = _video_mask,     SET=mask → VIDEO HIGH, CLR=0 (no-op)
 *   ビット=0 → _s = 0,               SET=0 (no-op),         CLR=mask → VIDEO LOW
 *
 * ブランチレス化の理由:
 *   if/else 条件分岐は Cortex-M0+ で branch-taken=3cy / not-taken=1cy。
 *   1ビット当たり ±1cy のばらつきが 8bit × 32col = 256bit で累積すると
 *   同じキャラクタの異なるスキャン行が異なる水平位置にずれ、文字形が崩れる。
 *   ブランチレスにより全ビット均一 17cy ≈ 136ns → 8bit × 17cy = 1.088µs/char。
 *
 * 総映像ウィンドウ: 32col × 1.088µs ≈ 34.8µs。
 * H-ライン内訳: 4µs HSYNC + 8µs back + 34.8µs active + 17.2µs front = 64µs。
 * ZephyrにAPIがないため SIO/TIMELR 直叩きを使う (RP2040 限定)。              */
#define _CVBS_PIXEL(p) do { \
    uint32_t _s = (uint32_t)(-(int32_t)((uint8_t)(p) >> 7)) & _video_mask; \
    _SIO_SET = _s; \
    _SIO_CLR = _video_mask ^ _s; \
    __asm__ volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"); \
} while (0)
uint32_t _sync_mask;    /* (1U << 16) for GPIO16 */
uint32_t _video_mask;   /* (1U << 17) for GPIO17 */

/* ── 状態変数 ───────────────────────────────────────────────────── */
volatile uint16_t _cvbs_line;
struct counter_alarm_cfg _cvbs_alarm;
bool _cvbs_on;
/* フォントパターン SRAM コピー。
 * XIP Flash (CHAR_PATTERN) はキャッシュミス時 ~80cy のペナルティがある。
 * ISR が呼ばれるたびに BASIC インタープリタが 16KB XIP キャッシュを
 * 入れ替えるため、スキャン行 0 でキャッシュミスが発生しやすく
 * 他の行との間にピクセル位置ずれ (字形崩れ) を引き起こす。
 * video_on() で SRAM にコピーして ISR 内の読み取りを確定的にする。       */
static uint8_t _cvbs_font[256 * 8];
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

    if (ln < 12u) {
        /* ── Vsync ライン: ロングパルス × 12ライン (IchigoJam_Q 仕様) ── */
        /* 12ライン連続でロングパルスを出し積分器を確実にトリガする。      */
        /* frames インクリメントは最初の 1ライン (ln==0) のみ。             */
        if (ln == 0u) { frames++; }
        _SIO_CLR = _sync_mask | _video_mask;   /* SYNC=L, VIDEO=L */
        _CVBS_WAIT_US(_CVBS_VSYNC_US);
        _SIO_SET = _sync_mask;                 /* SYNC=H */
        return;
    }

    /* ── 通常ライン: 水平同期 ───────────────────────────────────── */
    /* TIMELR ベースで H-sync + バックポーチを待機。alarm は 1MHz タイマー
     * 境界で発火するため ISR 開始時刻のライン間ばらつきは <128ns。
     * WAIT_US(N) の誤差は同一 µs tick 内の数十 ns に収まり、
     * 同一キャラクタの各スキャン行で水平位置が揃う。                    */
    _SIO_CLR = _sync_mask | _video_mask;       /* SYNC=L (hsync) */
    _CVBS_WAIT_US(_CVBS_HSYNC_US);
    _SIO_SET = _sync_mask;                     /* SYNC=H */
    _CVBS_WAIT_US(_CVBS_BACK_US);

    if (ln >= _CVBS_ACT_FIRST && ln < (uint16_t)(_CVBS_ACT_FIRST + _CVBS_ACT_LINES)) {
        /* ── アクティブライン: 映像出力 ─────────────────────────── */
        int vln = (int)ln - _CVBS_ACT_FIRST;
        int cy  = vln >> 3;   /* キャラクタ行 0..23 */
        int sr  = vln & 7;    /* キャラクタ内スキャン行 0..7 */

        const uint8_t *vrow = vram + cy * 32;

        /* PCG の先頭コード: SIZE_PCG=32 なら 256-32=224=0xE0。
         * IchigoJam は 0xE0-0xFF (= 32文字) をユーザー定義 PCG として使う。
         * screen_clp() で CHAR_PATTERN[0xE0*8..] → screen_pcg にコピー済み。
         * vram はゼロクリア (0x00=null) で初期化されるため、
         * 0x00 を CHAR_PATTERN ではなく screen_pcg で引くと 0xE0 の字形 (非空白) が
         * 表示されてしまう。0xE0 未満はすべて CHAR_PATTERN を参照すること。 */
        const uint8_t _pcg_first = (uint8_t)(256u - (uint32_t)SIZE_PCG); /* 0xE0 */

        for (int col = 0; col < 32; col++) {
            uint8_t ch  = vrow[col];
            /* ch >= 0xE0: user PCG, ch < 0xE0: standard font */
            uint8_t pat = (ch >= _pcg_first)
                ? screen_pcg[(ch - _pcg_first) * 8u + (uint8_t)sr]
                : _cvbs_font[ch * 8 + sr];  /* SRAM コピー: XIP キャッシュミス防止 */

            /* 8ビットを MSB 順にブランチレス出力。
             * _CVBS_PIXEL(p) は bit7 を出力し、`p <<= 1` で次ビットを MSB へ繰り上げる。
             * 全ビット均一 17cy = 136ns。8bit × 136ns = 1.088µs/char。          */
            uint8_t _p = pat;
            _CVBS_PIXEL(_p); _p = (uint8_t)(_p << 1);
            _CVBS_PIXEL(_p); _p = (uint8_t)(_p << 1);
            _CVBS_PIXEL(_p); _p = (uint8_t)(_p << 1);
            _CVBS_PIXEL(_p); _p = (uint8_t)(_p << 1);
            _CVBS_PIXEL(_p); _p = (uint8_t)(_p << 1);
            _CVBS_PIXEL(_p); _p = (uint8_t)(_p << 1);
            _CVBS_PIXEL(_p); _p = (uint8_t)(_p << 1);
            _CVBS_PIXEL(_p);
        }
        _SIO_CLR = _video_mask;                /* VIDEO=L (フロントポーチ) */
    }
}

/* ── 公開 API ────────────────────────────────────────────────────── */

INLINE void video_on(void) {
    SCREEN_W = 32;
    SCREEN_H = 24;
    if (_cvbs_on) { return; }

    /* CHAR_PATTERN (Flash/XIP) を SRAM にコピー。
     * ISR 内での Flash 読み取りはキャッシュミス時 ~80cy かかるため
     * スキャン行 0 だけピクセル位置がずれて字形が崩れる原因になる。
     * SRAM コピーにより全スキャン行の読み取り時間を均一化する。         */
    memcpy(_cvbs_font, CHAR_PATTERN, 256 * 8);

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
