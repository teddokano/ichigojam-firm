// IchigoJam_Z - CVBS video output (M6/M7)
// IchigoJam_Z - CVBS video output (M6/M7)
//
// 共通回路:
//   SYNC ─── 470Ω ──┬── CVBS出力 (75Ω終端テレビへ)
//   VIDEO─── 100Ω ──┘
//
// 信号レベル:
//   SYNC=L, VIDEO=L → 0V       (同期チップ)
//   SYNC=H, VIDEO=L → ~0.38V   (ブランク/黒)
//   SYNC=H, VIDEO=H → ~0.97V   (白)
//
// RP2040 (Pico):  M6 — GPIO16(SYNC) / GPIO17(VIDEO) / alarm0 (1MHz)
// MCXA153 (FRDM): M7 — P3_14/D9(SYNC) / P3_15/D8(VIDEO) / CTimer1 (1MHz)
//
// ISR 方式:
//   RP2040:   Zephyr counter alarm callback で 64µs 周期ライン ISR を生成。
//             ALARM0 直接書き込みで H-sync 周期を安定化。
//   MCXA153:  irq_connect_dynamic() で CTimer1 ISR を直接登録。
//             Zephyr counter driver preamble (~50-200cy 変動) をバイパスし、
//             alarm 発火から固定遅延 (IRQ latency + wrapper ~0.33µs) で
//             SYNC=L を出力 → H-sync ジッタを排除。
//
// 1ライン = 64µs (NTSC 63.5µs 近似)、261ライン/フレーム、≈60fps。
// フレーム構成: vsync(12) + vblank(21) + active(192) + vblank(36)。
//
// 非対応ボード: #if で全ブロックをガード。スタブ実装を提供。

#ifndef __DISPLAY_H__
#define __DISPLAY_H__

#include <zephyr/kernel.h>

#if defined(CONFIG_SOC_SERIES_RP2040) || defined(CONFIG_SOC_SERIES_MCXA1X3)
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/irq.h>

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ── 共通 NTSC タイミング定数 ─────────────────────────────────────────── */
#define _CVBS_LINES      261    /* 総ライン数 */
#define _CVBS_ACT_FIRST   33    /* アクティブ開始ライン (vsync12 + vblank21) */
#define _CVBS_ACT_LINES  192    /* アクティブライン数 (24行 × 8px) */
#define _CVBS_HTOTAL_US   64    /* 1ライン周期 µs */
#define _CVBS_HSYNC_US     4    /* 水平同期パルス幅 µs */
#define _CVBS_BACK_US      8    /* バックポーチ µs */
#define _CVBS_VSYNC_US    58    /* 垂直同期ロングパルス µs (58/64 = 91% duty) */

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ── 共通 GPIO / counter DTS バインディング ───────────────────────────── */
/* static を付けない: INLINE が non-static inline に展開される場合に
 * "static variable used in non-static inline function" 警告が出るため。
 * display.h は単一 TU にのみ include されるので多重定義の心配はない。  */
const struct gpio_dt_spec _cvbs_sync =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ij_cvbs_sync_gpios);
const struct gpio_dt_spec _cvbs_video =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ij_cvbs_video_gpios);
const struct device *_cvbs_ctr;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ── プラットフォーム固有レジスタ定義 ─────────────────────────────────── */

#if defined(CONFIG_SOC_SERIES_RP2040)
/* RP2040: SIO SET/CLR (高速 GPIO)、TIMELR (1MHz カウンタ)、ALARM0/ARMED
 *
 * SIO (0xD0000000):
 *   GPIO_OUT_SET +0x14 = 0xD0000014  ← Zephyr GPIO driver が内部使用、安全
 *   GPIO_OUT_CLR +0x18 = 0xD0000018
 *
 * TIMER (0x40054000):
 *   TIMELR  +0x0C = 0x4005400C  1MHz フリーランカウンタ下位32bit (RO)
 *   ALARM0  +0x10 = 0x40054010  アラーム0比較値 (書き込みでアーム)
 *   ARMED   +0x20 = 0x40054020  bit0: アーム中=1 / 発火済み=0 (RO)
 *
 * H-sync 周期安定化:
 *   counter_set_channel_alarm() は内部で TIMELR を読むが、
 *   XIP キャッシュミスで ±数百 cy 変動し H-sync 周期が不安定になる。
 *   callback 登録は Zephyr API で行い、実際の発火時刻は
 *   ALARM0 を直接書いて正確に制御する。
 * ZephyrにAPIがないため直叩きを使う (RP2040 限定)。                    */
#define _CVBS_HW_SET(m)       do { (*(volatile uint32_t *)0xD0000014U) = (m); } while (0)
#define _CVBS_HW_CLR(m)       do { (*(volatile uint32_t *)0xD0000018U) = (m); } while (0)
#define _CVBS_TIMER_TC        (*(volatile uint32_t *)0x4005400CU)
#define _CVBS_ALARM0_REG      (*(volatile uint32_t *)0x40054010U)
#define _CVBS_TIMER_ARMED     (*(volatile uint32_t *)0x40054020U)

/* ブランチレスピクセル出力 (RP2040)
 * SIO CLR=0 は no-op。全ビット均一 ~17cy ≈ 136ns @ 125MHz。
 * 10 NOPs: 8bit × 17cy = 1.088µs/char, 32col = ~34.8µs アクティブ。
 * ブランチレス理由: Cortex-M0+ branch-taken=3cy / not-taken=1cy
 *   → 1ビット ±1cy のばらつきが 256bit で累積し文字形が崩れる。        */
#define _CVBS_PIXEL(p) do { \
    uint32_t _s = (uint32_t)(-(int32_t)((uint8_t)(p) >> 7)) & _video_mask; \
    _CVBS_HW_SET(_s); \
    _CVBS_HW_CLR(_video_mask ^ _s); \
	__asm__ volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"); \
} while (0)

#elif defined(CONFIG_SOC_SERIES_MCXA1X3)
/* MCXA153: GPIO3 PSOR/PCOR (高速 GPIO)、CTimer1 TC (1MHz カウンタ)
 *
 * GPIO3 (0x40105000):
 *   PSOR +0x44 = 0x40105044  Set output high
 *   PCOR +0x48 = 0x40105048  Set output low (clear)
 *   PDOR +0x40 = 0x40105040  Data output register (DMA 転送先)
 *
 * CTimer1 (0x40005000):
 *   TC   +0x08 = 0x40005008  Timer Counter (prescale=95 → 1MHz = 1µs/tick)
 *
 * ZephyrにAPIがないため直叩きを使う (MCXA1X3 限定)。                   */
#define _CVBS_HW_SET(m)   do { (*(volatile uint32_t *)0x40105044U) = (m); } while (0)
#define _CVBS_HW_CLR(m)   do { (*(volatile uint32_t *)0x40105048U) = (m); } while (0)
#define _CVBS_TIMER_TC    (*(volatile uint32_t *)0x40005008U)
#define _GPIO3_PDOR       0x40105040U  /* GPIO3 Data Output Register (DMA 転送先) */

/* CTimer1 直接レジスタアクセス (H-sync ジッタ排除)
 *
 * 問題: Zephyr counter driver preamble (CTIMER_GetStatusFlags +
 *   CTIMER_ClearStatusFlags + read TC + callback dispatch) = ~50-200cy。
 *   I-cache の cold/warm 状態で変動し、_cvbs_line_cb() 呼び出しまでに
 *   ±数 µs のジッタが生じる → H-sync 立下りエッジが揺れる。
 *
 * 解決: irq_connect_dynamic() で _sw_isr_table[CTimer1_IRQ] を上書きし、
 *   直接 _cvbs_direct_isr() を登録する。
 *   z_arm_isr_wrapper は引き続き context save を行うが (~20cy 固定)、
 *   driver preamble は実行されない。
 *   SYNC=L が alarm 発火から固定遅延 (~0.33µs) で出力される。
 *
 * CTimer1 レジスタ:
 *   IR  +0x00 = 0x40005000  Interrupt Register   (bit0: MR0 match)
 *   MCR +0x14 = 0x40005014  Match Control Register (bit0: MR0I interrupt)
 *   MR0 +0x18 = 0x40005018  Match Register 0     (次 alarm 絶対時刻)   */
#define _CTIMER1_BASE  0x40005000U
#define _CTIMER1_IR    (*(volatile uint32_t *)(_CTIMER1_BASE + 0x00U))
#define _CTIMER1_MCR   (*(volatile uint32_t *)(_CTIMER1_BASE + 0x14U))
#define _CTIMER1_MR0   (*(volatile uint32_t *)(_CTIMER1_BASE + 0x18U))

/* CTimer2 レジスタ — ピクセルクロック (DMA trigger, M7)
 *
 * CTimer2 (0x40006000): prescale=0 → 96MHz、MR0=18 → 19cy/pixel ≈ 198ns
 *   IR  +0x00  Interrupt Register (bit0: MR0 match flag)
 *   TCR +0x04  Timer Control Register (bit0: CEN, bit1: CRST)
 *   MCR +0x14  Match Control Register (bit0: MR0I, bit1: MR0R)
 *   MR0 +0x18  Match Register 0
 *
 * 動作: MR0I=1, MR0R=1 → TC が 18 に達すると DMA request + TC リセット。
 *   NVIC IRQ 41 は video_on() で irq_disable() し CPU 割り込みを無効化する。
 *   DMA request は NVIC に依存せず、ハードウェア直接ルーティングで発生する。
 * ZephyrにAPIがないため直叩きを使う (MCXA1X3 限定)。                    */
#define _CTIMER2_BASE  0x40006000U
#define _CTIMER2_IR    (*(volatile uint32_t *)(_CTIMER2_BASE + 0x00U))
#define _CTIMER2_TCR   (*(volatile uint32_t *)(_CTIMER2_BASE + 0x04U))
#define _CTIMER2_MCR   (*(volatile uint32_t *)(_CTIMER2_BASE + 0x14U))
#define _CTIMER2_MR0   (*(volatile uint32_t *)(_CTIMER2_BASE + 0x18U))

/* DMA0 チャンネル3 レジスタ — ピクセル PDOR 転送 (M7)
 *
 * MCXA153 は EDMA3 IP。DMA0 ベース: 0x40080000。
 * 各チャンネルは 0x1000 オフセット: CH[n] = 0x40080000 + 0x1000 + n*0x1000。
 * CH3 = 0x40084000 (LPUART0/1/2 は CH0-2 を使用可能だが CONFIG では DMA 無効)。
 *
 * チャンネルオフセット (CH ベースから):
 *   CH_CSR  +0x00  Channel Control (bit0: ERQ, bit30: DONE(W1C))
 *   CH_MUX  +0x14  Channel MUX: DMA request source 35 = kDma0RequestMuxCtimer2M0
 *
 * TCD オフセット (CH ベースから):
 *   SADDR   +0x20  Source address
 *   SOFF    +0x24  Source offset (int16_t, +4 = 32bit increment)
 *   ATTR    +0x26  Transfer attributes (SSIZE/DSIZE: 2=32bit)
 *   NBYTES  +0x28  Minor loop byte count (4 bytes = 1 pixel)
 *   SLAST   +0x2C  Source last address adjustment (-(256*4) to reset)
 *   DADDR   +0x30  Destination address (GPIO3 PDOR = 0x40105040)
 *   DOFF    +0x34  Destination offset (0 = fixed)
 *   CITER   +0x36  Current major loop count (int16_t, 256)
 *   DLAST   +0x38  Destination last address adjustment (0)
 *   TCD_CSR +0x3C  TCD control (bit0: START SW trigger, bit3: DREQ)
 *   BITER   +0x3E  Beginning major loop count (256)
 *
 * 転送設定: 1 minor loop = 4 bytes = 1 pixel。Major loop = 256 回。
 *   DREQ=1: 256 転送完了後に ERQ を自動クリア (次ラインまで DMA 停止)。
 * ZephyrにAPIがないため直叩きを使う (MCXA1X3 限定)。                    */
#define _DMA_CH         3U
#define _DMA0_BASE      0x40080000U
#define _DMA_CH_BASE    (_DMA0_BASE + 0x1000U + (_DMA_CH) * 0x1000U)
#define _DMA_CH_CSR     (*(volatile uint32_t *)(_DMA_CH_BASE + 0x00U))
#define _DMA_CH_MUX     (*(volatile uint32_t *)(_DMA_CH_BASE + 0x14U))
#define _DMA_TCD_SADDR  (*(volatile uint32_t *)(_DMA_CH_BASE + 0x20U))
#define _DMA_TCD_SOFF   (*(volatile uint16_t *)(_DMA_CH_BASE + 0x24U))
#define _DMA_TCD_ATTR   (*(volatile uint16_t *)(_DMA_CH_BASE + 0x26U))
#define _DMA_TCD_NBYTES (*(volatile uint32_t *)(_DMA_CH_BASE + 0x28U))
#define _DMA_TCD_SLAST  (*(volatile uint32_t *)(_DMA_CH_BASE + 0x2CU))
#define _DMA_TCD_DADDR  (*(volatile uint32_t *)(_DMA_CH_BASE + 0x30U))
#define _DMA_TCD_DOFF   (*(volatile uint16_t *)(_DMA_CH_BASE + 0x34U))
#define _DMA_TCD_CITER  (*(volatile uint16_t *)(_DMA_CH_BASE + 0x36U))
#define _DMA_TCD_DLAST  (*(volatile uint32_t *)(_DMA_CH_BASE + 0x38U))
#define _DMA_TCD_CSR    (*(volatile uint16_t *)(_DMA_CH_BASE + 0x3CU))
#define _DMA_TCD_BITER  (*(volatile uint16_t *)(_DMA_CH_BASE + 0x3EU))

/* ピクセルバッファ: 2 ライン × 256 ピクセル × 4 bytes = 2KB
 * DMA が _vbuf[cur] を読む間、ISR が _vbuf[next] を展開 (ダブルバッファ)。
 * バッファ選択: cur = ln & 1 (現ラインの LSB)、next = cur ^ 1。           */
static uint32_t _vbuf[2][256];

/* PORT3 PCR (Pin Control Register) — CVBS GPIO スルーレート設定
 *
 * 問題: MCXA153 GPIO のデフォルトは SRE=0 (高速スルーレート)。
 *   急峻な立上り/立下りエッジ が 100Ω/470Ω 抵抗合成回路の寄生インダクタンス
 *   (PCB トレース + リード線) と共振し、CVBS 出力に大きなアンダーシュートを生じる。
 *
 * 解決: video_on() で PCR[14]/PCR[15] に SRE=1 (slow slew) を設定する。
 *   スルーレートを落としてアンダーシュートを抑制。
 *   DSE=0, DSE1=0 (low drive) で過剰な駆動電流も防ぐ。
 *   ZephyrにAPIがないため直叩きを使う (MCXA1X3 限定)。
 *
 * PORT3_BASE = 0x400BF000, PCR[n] at +0x80 + n×4
 *   PCR bit 3 = SRE: 0=fast slew (default), 1=slow slew
 *   PCR bit 6 = DSE: 0=low drive strength
 *   PCR bit 7 = DSE1: 0=low drive strength                              */
#define _PORT3_PCR_BASE  (0x400BF000U + 0x80U)
#define _PORT3_PCR(n)    (*(volatile uint32_t *)(_PORT3_PCR_BASE + (uint32_t)(n) * 4U))

/* DWT CYCCNT (Cortex-M33, 96MHz サイクルカウンタ)
 *
 * 問題: _CVBS_WAIT_UNTIL は CTimer1 TC (1MHz) を使用するため、スピンループ出口が
 *   TC クロック周期内 (96cy) のどこで発生するか不定 → ライン毎に 0-9cy のジッタ。
 *   96MHz で 9cy = 94ns ≈ 0.75px @ 12cy/pixel → 画面上の水平方向 ±1px のちらつき。
 *
 * 解決: DWT CYCCNT (96MHz) に切り替え、スピンループ出口ジッタを ~0-3cy (≈31ns
 *   ≈ 0.25px) に削減する。
 *   ISR 発火基準時刻 t0 = _DWT_CYCCNT (SYNC=L の直後) をアンカーとし、
 *   全タイミングを t0 からのサイクル数で指定する。
 *
 * 有効化: DEMCR.TRCENA=1 (bit24) → DWT.CTRL.CYCCNTENA=1 (bit0)。
 * ZephyrにAPIがないため直叩きを使う (MCXA1X3 限定)。                       */
#define _DWT_DEMCR   (*(volatile uint32_t *)0xE000EDFCU) /* CoreDebug->DEMCR */
#define _DWT_CTRL    (*(volatile uint32_t *)0xE0001000U) /* DWT->CTRL        */
#define _DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004U) /* DWT->CYCCNT      */

/* 96MHz サイクルカウンタ待機。signed 比較でラップアラウンドを正しく処理。  */
#define _CVBS_WAIT_UNTIL_CYC(t) do { \
    while ((int32_t)(_DWT_CYCCNT - (uint32_t)(t)) < 0) {} \
} while (0)

/* ブランチレスピクセル出力 (MCXA153)
 * GPIO3 PSOR/PCOR への AHB 書き込み + ブランチレスマスク計算。
 *
 * タイミング計算 (Cortex-M33 @ 96MHz):
 *   overhead (NOPなし) = 8cy/pixel:
 *     ldr r5(1) + ubfx(1,latency隠蔽) + mul(1) + str PSOR(1)
 *     + ldr r6(1) + load-use stall(1) + eors(1) + str PCOR(1) = 8cy
 *   目標: 22cy/pixel → NOP = 22-8 = 14
 *   14 NOPs: 22cy/pixel → 8px×22cy = 176cy/char = 1.833µs/char
 *     32col × 1.833µs = 58.7µs アクティブビデオ (N=13比+4.8%)
 *   ※ 多すぎれば文字幅が広がり、少なければ狭まる → 実機で微調整。   */
#define _CVBS_PIXEL(p) do { \
    uint32_t _s = (uint32_t)(-(int32_t)((uint8_t)(p) >> 7)) & _video_mask; \
    _CVBS_HW_SET(_s); \
    _CVBS_HW_CLR(_video_mask ^ _s); \
    __asm__ volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"); \
} while (0)

#endif /* platform */

/* ── 共通: 絶対時刻待機 ─────────────────────────────────────────────────
 * t = fire_time + オフセット(µs) で指定。
 * ISR entry の可変遅延に依らずタイミングを alarm 発火時刻に固定する。
 * signed 比較で uint32_t のラップアラウンドを正しく処理する。            */
#define _CVBS_WAIT_UNTIL(t) do { \
    while ((int32_t)(_CVBS_TIMER_TC - (uint32_t)(t)) < 0) {} \
} while (0)

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ── 共通 状態変数 ─────────────────────────────────────────────────────── */
uint32_t _sync_mask;              /* (1U << sync_pin) */
uint32_t _video_mask;             /* (1U << video_pin) */
volatile uint16_t _cvbs_line;
struct counter_alarm_cfg _cvbs_alarm;   /* RP2040 alarm callback 用 */
bool _cvbs_on;
/* フォントパターン SRAM コピー。
 * XIP Flash (CHAR_PATTERN) はキャッシュミス時 ~80cy のペナルティ。
 * BASIC インタープリタが 16KB XIP キャッシュを入れ替えるため、
 * スキャン行 0 でキャッシュミスが発生しピクセル位置がずれる。
 * video_on() で SRAM にコピーして全スキャン行の読み取りを均一化。       */
static uint8_t _cvbs_font[256 * 8];
/* 次ライン ISR の「本来の」絶対発火時刻 (1MHz カウンタ値 = µs)。
 * ISR が遅延しても次アラームを本来の時刻から計算し
 * 長期的な H-sync 周期を 64µs に安定させる。                           */
volatile uint32_t _cvbs_next_line_time;

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ── RP2040: alarm スケジュール + callback ISR ───────────────────────── */
#if defined(CONFIG_SOC_SERIES_RP2040)

/* RP2040: counter driver の ABSOLUTE バグ回避
 *   driver bug: ABSOLUTE 設定時 target=0 → alarm_at=now → 常に -ETIME。
 *   対策: 相対 ticks=64µs で callback のみ登録し (-ETIME 回避)、
 *   直後に ALARM0 を絶対時刻で上書きして正確な発火時刻を指定する。      */
static inline void _cvbs_sched_alarm(const struct device *dev, uint8_t chan)
{
    _cvbs_alarm.ticks = _CVBS_HTOTAL_US; /* callback 登録目的; 64µs先 → -ETIME不発生 */
    counter_set_channel_alarm(dev, chan, &_cvbs_alarm);
    /* ALARM0 を目標絶対時刻で上書き (driver 内部の可変 TIMELR 読み取りをバイパス) */
    _CVBS_ALARM0_REG = _cvbs_next_line_time;
    /* ARMED=0: 書いた時刻が既に過去 → 1周期後に回復 */
    if (!(_CVBS_TIMER_ARMED & 1u)) {
        _cvbs_next_line_time = _CVBS_TIMER_TC + _CVBS_HTOTAL_US;
        _CVBS_ALARM0_REG = _cvbs_next_line_time;
    }
}

/* RP2040 ライン ISR (counter alarm callback)
 *
 * タイミング設計:
 *   ① SYNC=L を ISR の最初の1命令でアサート。
 *   ② fire_time = _cvbs_next_line_time (インクリメント前) を基準とする。
 *      ticks はドライバが読んだ TC 値で ±1µs ジッタがあるため使用しない。
 *   ③ _CVBS_WAIT_UNTIL(fire_time + N) で全タイミングを alarm 発火時刻に固定。
 *   alarm IRQ 優先度 = 0 (最高、overlay で設定) により
 *   Zephyr カーネル tick が CVBS ISR を割り込めない。                   */
void _cvbs_line_cb(const struct device *dev, uint8_t chan,
                   uint32_t ticks, void *user_data)
{
    ARG_UNUSED(user_data);
    ARG_UNUSED(ticks);

    _CVBS_HW_CLR(_sync_mask | _video_mask);      /* SYNC=L, VIDEO=L */

    uint32_t fire_time = _cvbs_next_line_time;    /* alarm 発火時刻 (ジッタゼロ) */
    _cvbs_next_line_time += _CVBS_HTOTAL_US;
    _cvbs_sched_alarm(dev, chan);                 /* 次ライン alarm スケジュール */

    uint16_t ln = _cvbs_line;
    _cvbs_line = (ln + 1u < _CVBS_LINES) ? (uint16_t)(ln + 1u) : 0u;
    _g.linecnt++;

    if (ln < 12u) {
        if (ln == 0u) { frames++; }
        _CVBS_WAIT_UNTIL(fire_time + _CVBS_VSYNC_US);
        _CVBS_HW_SET(_sync_mask);
        return;
    }

    _CVBS_WAIT_UNTIL(fire_time + _CVBS_HSYNC_US);
    _CVBS_HW_SET(_sync_mask);
    _CVBS_WAIT_UNTIL(fire_time + _CVBS_HSYNC_US + _CVBS_BACK_US);

    if (ln >= _CVBS_ACT_FIRST && ln < (uint16_t)(_CVBS_ACT_FIRST + _CVBS_ACT_LINES)) {
        int vln = (int)ln - _CVBS_ACT_FIRST;
        int cy  = vln >> 3;
        int sr  = vln & 7;
        const uint8_t *vrow = vram + cy * 32;
        const uint8_t _pcg_first = (uint8_t)(256u - (uint32_t)SIZE_PCG);

        for (int col = 0; col < 32; col++) {
            uint8_t ch  = vrow[col];
            uint8_t pat = (ch >= _pcg_first)
                ? screen_pcg[(ch - _pcg_first) * 8u + (uint8_t)sr]
                : _cvbs_font[ch * 8 + sr];

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
        _CVBS_HW_CLR(_video_mask);
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ── MCXA153: 直接 ISR (H-sync ジッタ排除) ───────────────────────────── */
#elif defined(CONFIG_SOC_SERIES_MCXA1X3)

/* CTimer1 TC (1µs) と DWT CYCCNT (96MHz) の位相差。
 * CYCCNT = TC * 96 + _cvbs_cy_phase (定数)。
 * video_on() で測定し、ISR 内で fire_time(µs) → CYCCNT target 変換に使用。
 *
 * ジッタ排除の仕組み:
 *   _CVBS_WAIT_UNTIL_CYC(_cvbs_cy_phase + (fire_time + N) * 96U) は
 *   「TC が fire_time+N になる瞬間の CYCCNT」を直接計算して待機する。
 *   ISR latency (IRQ遅延・LDM stall 等の変動) に依らずタイミングが固定される。
 *   exit 精度 ~0-3cy (≈31ns ≈ 0.25px @ 18cy/pixel)。                   */
static uint32_t _cvbs_cy_phase;

/* _expand_line: 1ラインのピクセル PDOR 値を buf[256] に展開する (M7)
 *
 * buf      : 転送先 uint32_t[256] (_vbuf[0] or _vbuf[1])
 * vrow     : vram の対象行先頭ポインタ (32 文字)
 * sr       : スキャン行 (0-7)
 * pdor_base: GPIO3 PDOR の現在値から sync/video ビットを除いた値
 *            (OUT ピン状態を black/white ピクセル値に反映するため)
 *
 * 実行時間: ~2048cy @ 96MHz ≈ 21µs。
 * __ramfunc: _cvbs_direct_isr (SRAM) から呼ばれるため Flash I-cache ミスを防ぐ。
 * DMA が _vbuf[cur] を転送中に _vbuf[next] (別バッファ) を展開するため競合なし。 */
static void __attribute__((section(".ramfunc")))
_expand_line(uint32_t *buf, const uint8_t *vrow, int sr, uint32_t pdor_base)
{
    const uint8_t pcg_first = (uint8_t)(256u - (uint32_t)SIZE_PCG);
    uint32_t black = pdor_base | _sync_mask;
    uint32_t white = black | _video_mask;
    for (int col = 0; col < 32; col++) {
        uint8_t ch  = vrow[col];
        uint8_t pat = (ch >= pcg_first)
            ? screen_pcg[(ch - pcg_first) * 8u + (uint8_t)sr]
            : _cvbs_font[ch * 8 + (uint8_t)sr];
        /* bit7 (MSB) が画面左端。DMA は buf[0] から順に GPIO3 PDOR へ書くため
         * MSB → LSB の順に格納する。                                       */
        for (int b = 7; b >= 0; b--) {
            buf[col * 8 + (7 - b)] = (pat >> b & 1) ? white : black;
        }
    }
}

/* MCXA153 直接 ライン ISR
 *
 * irq_connect_dynamic() により _sw_isr_table[CTimer1_IRQ] に登録される。
 * z_arm_isr_wrapper 経由で呼ばれるが driver preamble は実行されない。
 *
 * タイミング設計:
 *   ① cy_phase + fire_time*96 + 48 まで待機 → SYNC=L アサート (絶対時刻固定)
 *   ② SYNC=L 直後に t_sync_l = CYCCNT をキャプチャ
 *   ③ 以降の全タイミングを t_sync_l 基準で指定する
 *
 * ③ の理由:
 *   wrapper latency (60-160cy 可変) で SYNC=L が ① の目標時刻より遅れた場合、
 *   cy_phase+(fire_time+N)*96 は既に過去 → WAIT が即時 exit → SYNC=H・VIDEO が
 *   SYNC=L エッジから N µs ではなく「待機ゼロ」でアサートされる。
 *   スコープが SYNC=L でトリガするため VIDEO の相対位置が毎ライン変わって見える。
 *   t_sync_l 基準にすれば wrapper latency に関わらず VIDEO 位置が SYNC=L から
 *   常に固定距離となり、スコープ上のジッタが消える。
 *
 * __ramfunc (section(".ramfunc")): ISR 全体を SRAM から実行する。
 *   vblank 期間に BASIC インタープリタが Flash I-cache を入れ替えるため、
 *   アクティブライン先頭でキャッシュミス (~50-100cy) が発生する問題を解消。
 *   ZephyrにAPIがないため直叩きを使う (MCXA1X3 限定)。                   */
static void __attribute__((section(".ramfunc"))) _cvbs_direct_isr(const void *arg)
{
    ARG_UNUSED(arg);

    /* ── ① SYNC=L を fire_time 基準の固定絶対時刻にアサート ────────── */
    uint32_t fire_time = _cvbs_next_line_time;   /* scheduled 発火時刻 (1µs) */
    _CVBS_WAIT_UNTIL_CYC(_cvbs_cy_phase + fire_time * 96U + 48U);
    _CVBS_HW_CLR(_sync_mask | _video_mask);      /* SYNC=L */

    /* ② SYNC=L エッジ直後の CYCCNT をキャプチャ。
     *   以降の全タイミング (HSYNC, backporch, pixel) をここからの相対値で指定。
     *   ISR latency / wrapper latency の変動が pixel 位置に影響しない。
     *   _DWT_CYCCNT 読み取りは PPB レイテンシ ~2cy を含むため、
     *   t_sync_l = SYNC=L アサートから約 2cy 後の値。定数オフセットなので問題なし。 */
    uint32_t t_sync_l = _DWT_CYCCNT;

    /* ── ③ MR0 割り込みフラグクリア + 次 alarm 設定 ───────────────── */
    _CTIMER1_IR = 0x01U;
    _cvbs_next_line_time += _CVBS_HTOTAL_US;
    _CTIMER1_MR0 = _cvbs_next_line_time;

    uint16_t ln = _cvbs_line;
    _cvbs_line = (ln + 1u < _CVBS_LINES) ? (uint16_t)(ln + 1u) : 0u;
    _g.linecnt++;

    /* ── ④ ライン種別に応じた映像出力 ──────────────────────────────── */
    if (ln < 12u) {
        if (ln == 0u) { frames++; }
        _CVBS_WAIT_UNTIL_CYC(t_sync_l + (uint32_t)_CVBS_VSYNC_US * 96U);
        _CVBS_HW_SET(_sync_mask);
        return;
    }

    _CVBS_WAIT_UNTIL_CYC(t_sync_l + (uint32_t)_CVBS_HSYNC_US * 96U);
    _CVBS_HW_SET(_sync_mask);

    /* ── アクティブライン判定 ────────────────────────────────────────────── */
    bool _act = (ln >= _CVBS_ACT_FIRST &&
                 ln <  (uint16_t)(_CVBS_ACT_FIRST + _CVBS_ACT_LINES));

    _CVBS_WAIT_UNTIL_CYC(t_sync_l + (uint32_t)(_CVBS_HSYNC_US + _CVBS_BACK_US) * 96U);

    /* pdor_base: 現在の GPIO3 PDOR から sync/video ビットを除いた値。
     * _expand_line に渡し black/white ピクセル値に OUT ピン状態を反映させる。
     * バックポーチ終了直後に取得 (SYNC=H, VIDEO=L の状態)。               */
    uint32_t _pdor_base = (*(volatile uint32_t *)_GPIO3_PDOR)
                          & ~(_sync_mask | _video_mask);

    if (_act) {
        int _cur      = (int)(ln & 1u);
        int _next_buf = _cur ^ 1;

        /* DMA re-arm: _vbuf[_cur] の 256 ピクセルを GPIO3 PDOR へ自律転送。
         *
         * 手順:
         *   1. SADDR を現バッファ先頭に更新
         *   2. DONE フラグクリア (CH_CSR bit30 W1C: 1 を書くとクリア)
         *   3. ERQ=1: DMA request を受け付ける (DREQ=1 で前ラインの完了時にクリアされた)
         *   4. CTimer2 CRST→CEN: ピクセルクロックをバックポーチ終了に同期してリスタート
         *   5. TCD_CSR.START=1: pixel[0] をソフトウェアトリガーで即時発火
         *      以降 pixel[1]-pixel[255] は CTimer2 Match0 (19cy ≈ 198ns 毎) でトリガ。
         *
         * DMA 転送時間: 256 × 198ns ≈ 50µs。次ライン ISR (64µs後) までに完了。
         * DREQ=1 (video_on() で設定済み): major loop 完了後 ERQ 自動クリア。   */
        _DMA_TCD_SADDR = (uint32_t)_vbuf[_cur];
        _DMA_CH_CSR    = (1U << 30);                         /* clear DONE (W1C) */
        _DMA_CH_CSR   |= (1U << 0);                          /* ERQ=1            */
        _CTIMER2_TCR   = 0x02U;                              /* CRST=1: reset    */
        _CTIMER2_TCR   = 0x01U;                              /* CEN=1: start     */
        _DMA_TCD_CSR   = (uint16_t)((1U << 3) | (1U << 0)); /* DREQ=1, START=1  */

        /* 次アクティブライン用バッファを DMA と並行展開。
         * DMA は _vbuf[_cur] を読み、_expand_line は _vbuf[_next_buf] に書く。
         * 異なるバッファへのアクセスのため競合なし (~21µs で完了, DMA より先に終わる)。 */
        int _next_ln = (int)ln + 1;
        if (_next_ln < _CVBS_ACT_FIRST + _CVBS_ACT_LINES) {
            int _nvln = _next_ln - _CVBS_ACT_FIRST;
            _expand_line(_vbuf[_next_buf],
                         vram + (_nvln >> 3) * 32,
                         _nvln & 7, _pdor_base);
        }
    } else if (ln == (uint16_t)(_CVBS_ACT_FIRST - 1u)) {
        /* ln=32: 次ライン (ln=33) がアクティブ開始。
         * ln=33 の cur = 33 & 1 = 1 なので _vbuf[1] を先行展開。
         * ln=33 の ISR は _vbuf[1] が展開済みであることを前提に DMA を起動する。 */
        _expand_line(_vbuf[_CVBS_ACT_FIRST & 1u],
                     vram, 0, _pdor_base);
    }
}

#endif /* RP2040 / MCXA1X3 ISR */

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * ── 共通 公開 API ─────────────────────────────────────────────────────── */

INLINE void video_on(void) {
    SCREEN_W = 32;
    SCREEN_H = 24;
    if (_cvbs_on) { return; }

    /* CHAR_PATTERN (Flash/XIP) を SRAM にコピー。
     * ISR 内での Flash 読み取りはキャッシュミス時 ~80cy。
     * SRAM コピーにより全スキャン行の読み取り時間を均一化。             */
    memcpy(_cvbs_font, CHAR_PATTERN, 256 * 8);

    gpio_pin_configure_dt(&_cvbs_sync,  GPIO_OUTPUT_LOW);
    gpio_pin_configure_dt(&_cvbs_video, GPIO_OUTPUT_LOW);

#if defined(CONFIG_SOC_SERIES_MCXA1X3)
    /* PORT3 PCR: slow slew rate, low drive strength for CVBS GPIO (P3_14/P3_15).
     * gpio_pin_configure_dt() の後に設定する (GPIO driver が PCR を上書きする場合に備え)。
     * SRE=1(bit3), DSE=0(bit6), DSE1=0(bit7) を書き込む。
     * ピン番号は DTS の gpio spec から取得 (P3_14=pin14, P3_15=pin15)。        */
    _PORT3_PCR((uint32_t)_cvbs_sync.pin)  =
        (_PORT3_PCR((uint32_t)_cvbs_sync.pin)  & ~(0x40U | 0x80U)) | 0x08U;
    _PORT3_PCR((uint32_t)_cvbs_video.pin) =
        (_PORT3_PCR((uint32_t)_cvbs_video.pin) & ~(0x40U | 0x80U)) | 0x08U;
#endif

    _sync_mask  = (1U << (uint32_t)_cvbs_sync.pin);
    _video_mask = (1U << (uint32_t)_cvbs_video.pin);

    _cvbs_ctr = DEVICE_DT_GET(DT_CHOSEN(ij_cvbs_timer));
    _cvbs_line = 0u;

    uint32_t _t0;
    counter_get_value(_cvbs_ctr, &_t0);
    _cvbs_next_line_time = _t0 + _CVBS_HTOTAL_US;

#if defined(CONFIG_SOC_SERIES_RP2040)
    /* RP2040: counter alarm callback で ISR を起動する。
     * ABSOLUTE driver バグのため相対モード (flags=0) で初回発火。        */
    _cvbs_alarm.callback  = _cvbs_line_cb;
    _cvbs_alarm.user_data = NULL;
    _cvbs_alarm.flags = 0u;
    _cvbs_alarm.ticks = _CVBS_HTOTAL_US;
    counter_start(_cvbs_ctr);
    counter_set_channel_alarm(_cvbs_ctr, 0, &_cvbs_alarm);

#elif defined(CONFIG_SOC_SERIES_MCXA1X3)
    /* DWT CYCCNT 有効化 (per-line 水平ジッタ排除)。
     * CYCCNT (96MHz) を ISR 内タイミング基準として使用し、
     * CTimer1 TC (1MHz) スピンループの 0-9cy 出口ジッタを排除する。
     * DEMCR.TRCENA=1 (bit24): DWT/ITM/ETM へのクロック供給を有効化。
     * DWT.CTRL.CYCCNTENA=1 (bit0): CYCCNT を 96MHz でカウント開始。
     * ZephyrにAPIがないため直叩きを使う (MCXA1X3 限定)。              */
    _DWT_DEMCR  |= (1UL << 24);  /* TRCENA */
    _DWT_CYCCNT  = 0U;           /* カウンタリセット */
    _DWT_CTRL   |= (1UL << 0);   /* CYCCNTENA */

    /* MCXA153: irq_connect_dynamic() で CTimer1 ISR を直接登録。
     *
     * driver init 済み (CTIMER_Init: clock enable, PR=95, TC=0, stopped)。
     * counter_start() の前に MR0/MCR を設定することで
     * TC 開始直後から正確な 64µs 周期で alarm が発火する。
     *
     * _sw_isr_table[CTimer1_IRQ] を _cvbs_direct_isr で上書きするため
     * CONFIG_DYNAMIC_INTERRUPTS=y が必要 (prj.conf 参照)。
     * IRQ の enable は driver init 時に完了済みのため irq_enable() 不要。 */
    _CTIMER1_MR0 = _cvbs_next_line_time;          /* 初回発火時刻を設定 */
    _CTIMER1_MCR = 0x01U;                          /* MR0I: MR0 match で interrupt */
    counter_start(_cvbs_ctr);                      /* TC カウント開始 */
    /* TC と CYCCNT の位相差を測定: cy_phase = CYCCNT - TC*96。
     * double-read で TC 境界またぎを排除 (誤差 ≤2cy → 水平位置誤差 ≤0.17px)。 */
    {
        uint32_t _ta, _tb, _cy;
        do { _ta = _CVBS_TIMER_TC; _cy = _DWT_CYCCNT; _tb = _CVBS_TIMER_TC; } while (_ta != _tb);
        _cvbs_cy_phase = _cy - _ta * 96U;
    }
    /* irq_connect_dynamic(): NVIC priority 0 (IRQ_ZERO_LATENCY) に設定し IRQ を有効化。
     * _sw_isr_table への登録は行われるが、次のベクタテーブル上書きで使われなくなる。 */
    irq_connect_dynamic(
        DT_IRQ(DT_CHOSEN(ij_cvbs_timer), irq),
        0, _cvbs_direct_isr, NULL, IRQ_ZERO_LATENCY);

    /* ARM ベクタテーブル (SRAM) に直接 _cvbs_direct_isr を登録。
     *
     * 問題: z_arm_isr_wrapper (Flash) はキャッシュミス時 +50-100cy の可変遅延。
     *   BASIC インタープリタが Flash I-cache を置き換えると wrapper がコールドになり、
     *   SYNC=L エッジが 48cy の余裕を超えてずれ → ライン毎の水平ジッタが残る。
     *
     * 解決: CONFIG_SRAM_VECTOR_TABLE=y (prj.conf) でベクタテーブルを SRAM に移動。
     *   CTimer1 IRQ エントリを直接 _cvbs_direct_isr に上書きし wrapper を完全排除。
     *   ISR latency = CM33 ハードウェア固定 12cy + LDM stall 0-9cy のみ。
     *   -> 48cy マージンで確実に吸収 → SYNC=L ジッタ ≈ 0cy。
     *
     * SCB->VTOR (0xE000ED08): ベクタテーブルのベースアドレス。
     *   エントリ[16 + irq] に Thumb アドレス (bit0=1) を書き込む。
     *   _cvbs_direct_isr は bare CM33 exception handler として動作:
     *     CM33 が context save/restore を自動実行、Zephyr wrapper 不要。
     *   ZephyrにAPIがないため直叩きを使う (MCXA1X3 限定)。                 */
    {
        uint32_t _irqn = DT_IRQ(DT_CHOSEN(ij_cvbs_timer), irq);
        volatile uint32_t *_vt =
            (volatile uint32_t *)(*(volatile uint32_t *)0xE000ED08U); /* SCB->VTOR */
        _vt[16u + _irqn] = ((uint32_t)_cvbs_direct_isr) | 1U;        /* Thumb bit */
        /* ARM barrier: ベクタテーブル書き込みを確実に完了させ新エントリを即時有効化。
         * DSB: STR がメモリに到達するまで後続命令を待機 (store buffer flush)。
         * ISB: パイプライン/プリフェッチをフラッシュ → 次の例外は新エントリを参照。
         * これを省くと I-cache/store buffer に旧エントリが残り wrapper が呼ばれ続ける。
         * ZephyrにAPIがないため直叩きを使う (MCXA1X3 限定)。               */
        __asm__ volatile ("dsb" ::: "memory");
        __asm__ volatile ("isb" ::: "memory");
    }

    /* M7: CTimer2 + DMA0 CH3 初期化
     *
     * CTimer2 はピクセルクロックとして使用。prescale=0 (96MHz)、MR0=18 で
     * 19cy ≈ 198ns/pixel。ISR 内でアクティブライン毎にリセット・起動する。
     * IRQ 41 を無効化: DMA request は NVIC を経由しないためハードウェア直結で動作。
     *
     * DMA0 CH3 TCD: 固定フィールドを設定。SADDR は ISR で毎ライン更新する。
     * CH_MUX=35 (kDma0RequestMuxCtimer2M0): CTimer2 Match0 → DMA CH3 trigger。
     * ZephyrにAPIがないため直叩きを使う (MCXA1X3 限定)。                    */
    irq_disable(41);
    _CTIMER2_MCR = 0x02U;  /* MR0R=1: TC を MR0 一致でリセット。MR0I=0 (CPU 割り込みなし) */
    _CTIMER2_MR0 = 18U;    /* 19cy/pixel @ 96MHz ≈ 198ns/pixel                           */
    _CTIMER2_TCR = 0x00U;  /* 停止状態 (ISR が毎ラインリセット・起動する)                */

    /* DMA0 CH3: trigger source = CTimer2 Match0 */
    _DMA_CH_MUX = 35U;  /* kDma0RequestMuxCtimer2M0 = 35 (fsl_inputmux_connections.h) */

    /* TCD 固定フィールド: 毎ライン変化しない設定を一度だけ書き込む */
    _DMA_TCD_SOFF   = (uint16_t)4U;               /* SOFF: +4 bytes per transfer (32bit) */
    _DMA_TCD_ATTR   = (uint16_t)((2U << 8) | 2U); /* SSIZE=2(32bit), DSIZE=2(32bit)      */
    _DMA_TCD_NBYTES = 4U;                          /* 1 minor loop = 4 bytes = 1 pixel    */
    _DMA_TCD_SLAST  = 0U;                          /* SLAST=0 (SADDR は ISR で毎回設定)   */
    _DMA_TCD_DADDR  = _GPIO3_PDOR;                 /* 転送先: GPIO3 PDOR (固定)            */
    _DMA_TCD_DOFF   = (uint16_t)0U;               /* DOFF=0 (宛先は固定アドレス)          */
    _DMA_TCD_DLAST  = 0U;                          /* DLAST=0 (宛先は固定)                 */
    _DMA_TCD_BITER  = (uint16_t)256U;             /* major loop 開始値 = 256 pixels       */
    _DMA_TCD_CITER  = (uint16_t)256U;             /* 現在値 (BITER と一致させる)           */
    _DMA_TCD_CSR    = (uint16_t)(1U << 3);        /* DREQ=1: major loop 完了後 ERQ 自動クリア */

    /* _vbuf[1] を先行展開: 最初のアクティブライン ln=33 (cur = 33&1 = 1) 用。
     * ln=32 の ISR の else branch でも展開されるが、video_on() が
     * フレーム中途から呼ばれた場合に ln=32 ISR を逃す可能性があるため
     * ここでも展開しておく (二重展開は無害: 同一内容で上書きされるだけ)。   */
    {
        uint32_t _pdor = (*(volatile uint32_t *)_GPIO3_PDOR)
                         & ~(_sync_mask | _video_mask);
        _expand_line(_vbuf[_CVBS_ACT_FIRST & 1u], vram, 0, _pdor);
    }
#endif

    _cvbs_on = true;
}

INLINE void video_off(int clkdiv) {
    ARG_UNUSED(clkdiv);
    if (!_cvbs_on) { return; }
    /* counter_cancel_channel_alarm: CTIMER_DisableInterrupts(base, 1) →
     * MCR bit0 をクリアし MR0 割り込みを停止する。
     * RP2040/MCXA153 ともに driver API 経由で安全に停止できる。          */
    counter_cancel_channel_alarm(_cvbs_ctr, 0);
#if defined(CONFIG_SOC_SERIES_MCXA1X3)
    /* M7: CTimer2 停止 + DMA CH3 無効化 */
    _CTIMER2_TCR = 0x00U;  /* CEN=0, CRST=0: CTimer2 停止                    */
    _DMA_CH_CSR  = 0U;     /* ERQ=0: DMA チャンネル無効化 (実行中なら完了まで継続) */
#endif
    _CVBS_HW_CLR(_sync_mask | _video_mask);
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
 * CLAUDE.md の制約: 最低 16ms の yield を保証する。                    */
INLINE void video_waitSync(uint n) {
    if (n == 0u) { return; }
    if (_cvbs_on) {
        uint16_t target = (uint16_t)((uint16_t)frames + (uint16_t)n);
        while ((int16_t)(target - frames) > 0) {
            k_sleep(K_MSEC(1));
        }
    } else {
        k_sleep(K_MSEC((uint32_t)n * 1000u / 60u));
    }
}

#else /* !RP2040 && !MCXA1X3 */

/* ── 非対応ボード: CVBS なし ─────────────────────────────────────────
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

INLINE void video_waitSync(uint n) {
    if (n > 0u) {
        k_sleep(K_MSEC((uint32_t)n * 1000u / 60u));
    }
}

#endif /* CONFIG_SOC_SERIES_RP2040 || CONFIG_SOC_SERIES_MCXA1X3 */

#endif /* __DISPLAY_H__ */
