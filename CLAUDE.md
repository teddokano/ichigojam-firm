# IchigoJam_Z — M7作業用 CLAUDE.md

## 最重要指示

- **作業対象: `IchigoJam_Z/` 以下のみ**
- **`IchigoJam_BASIC/` は読まない・変更しない**
- **`IchigoJam_P/` `IchigoJam_MCX/` `console/` は参照のみ**
- **1タスク1指示。複数ファイルを同時に変更しない**
- **変更のたびにビルド確認してから次へ進む**
- **`main` ブランチへのマージ禁止。作業ブランチ: `zephyr-m6`（M7完了時に `zephyr-m7` としてコミット）**

---

## 現在の状態

M0〜M6完了。M7はFRDM-MCXA153でのCVBS DMA実装。

| M | 内容 | 状態 |
|---|---|---|
| M0〜M6 | UART/GPIO/NVS/PWM/PSG/I2C/USR/CVBS | 完了 |
| M7 | CVBSビデオ出力（CTimer+DMA、MCXA153） | **作業中** |

---

## M7作業開始時に最初にやること（順番厳守）

### ステップ0（最初に必ず）: `sound.h` の二重インクリメントバグ修正

```c
// sound.h の _sound_timer_fn 内
// 修正前:
#if !defined(CONFIG_SOC_SERIES_RP2040)
    frames++;
    _g.linecnt++;
#endif

// 修正後:
#if !defined(CONFIG_SOC_SERIES_RP2040) && \
    !defined(CONFIG_SOC_SERIES_MCXA1X3)
    frames++;
    _g.linecnt++;
#endif
```

MCXA153では `_cvbs_direct_isr` も `frames++`/`linecnt++` を実行するため
現状は二重インクリメントになっている。`TICK(0)` が2倍速になるバグ。

**ステップ0完了後: `west build -b frdm_mcxa153` でビルド確認**

---

## M7 実装計画

### 何をするか（1文で）

`_cvbs_direct_isr` 内のピクセルビジーループ（`_CVBS_PIXEL` × 256）を
CTimer2 + DMA0 による自律転送に置き換える。

### 採用設計

**DMA転送方式: GPIO3 PDOR への 1転送 = 1ピクセル**

```
DMAバッファ[i] = GPIO3 PDOR の32bit値
  白: pdor_base | sync_mask | video_mask
  黒: pdor_base | sync_mask
転送先: GPIO3->PDOR = 0x40105040（アドレス固定）
転送数: 256 / ライン
```

**タイミング:**
- CTimer2: prescale=0, Match0=19 → 20cy = 208ns/pixel
- 256転送 × 208ns = 53µs（有効映像期間52µs に収まる）
- DMA完了から次ラインまで: 64µs - 53µs = 11µs 余裕

**バッファ:**
```c
static uint32_t _vbuf[2][256];  // 2KB（ダブルバッファ）
```

**展開（メインスレッドで実行）:**
```c
void _expand_line(int next, const uint8_t *vrow, int sr,
                  uint32_t pdor_base) {
    const uint8_t pcg_first = (uint8_t)(256u - SIZE_PCG);
    uint32_t black = pdor_base | _sync_mask;
    uint32_t white = black | _video_mask;
    for (int col = 0; col < 32; col++) {
        uint8_t ch  = vrow[col];
        uint8_t pat = (ch >= pcg_first)
            ? screen_pcg[(ch - pcg_first) * 8 + sr]
            : _cvbs_font[ch * 8 + sr];
        for (int b = 7; b >= 0; b--)
            _vbuf[next][col * 8 + (7 - b)] =
                (pat >> b & 1) ? white : black;
    }
}
```

展開時間: 約21µs（2048cy @ 96MHz）。
バックポーチ8µs(768cy)に収まらないため**メインスレッドで先行展開**する。

**展開フラグ（ISRとメインスレッドの通信）:**
```c
static volatile bool     _expand_req;   // ISRがtrueを書く
static volatile int      _expand_next;  // 次バッファ番号（0 or 1）
static volatile uint8_t *_expand_vrow;  // 展開対象行ポインタ
static volatile int      _expand_sr;   // スキャン行 (0-7)

// video_waitSync() 内（n==0でも確認）:
if (_cvbs_on && _expand_req) {
    uint32_t base = (*(volatile uint32_t *)0x40105040U)
                    & ~(_sync_mask | _video_mask);
    _expand_line(_expand_next, (const uint8_t *)_expand_vrow,
                 _expand_sr, base);
    _expand_req = false;  // 展開完了後にクリア（順序重要）
}
```

### ISR内の変更（`_cvbs_direct_isr` 内、アクティブライン処理部分）

削除:
```c
// ビジーループ全体（約60行）を削除
for (int col = 0; col < 32; col++) { ... _CVBS_PIXEL ... }
_CVBS_HW_CLR(_video_mask);
```

追加（バックポーチ終了アンカーの直後）:
```c
// バッファ番号を _cvbs_line の偶奇で決定
int cur  = (_cvbs_line & 1u) ? 1 : 0;
int next = cur ^ 1;

// DMA を現バッファで起動（ソフトウェアトリガーで1発目）
_ctimer2_start_and_dma_trigger(cur);  // 詳細は下記

// 次ライン展開をメインスレッドに依頼
if (_act) {
    _expand_next = next;
    _expand_vrow = _vrow;
    _expand_sr   = _sr;
    _expand_req  = true;
}
```

### ハードウェア構成（直叩き部分）

すべて `#if defined(CONFIG_SOC_SERIES_MCXA1X3)` で囲む。

**CTimer2 レジスタ（ベース: 0x40006000）:**
```c
#define _CTIMER2_BASE 0x40006000U
#define _CTIMER2_TCR  (*(volatile uint32_t *)(_CTIMER2_BASE + 0x04U))
#define _CTIMER2_PR   (*(volatile uint32_t *)(_CTIMER2_BASE + 0x0CU))
#define _CTIMER2_MR0  (*(volatile uint32_t *)(_CTIMER2_BASE + 0x18U))
#define _CTIMER2_MCR  (*(volatile uint32_t *)(_CTIMER2_BASE + 0x14U))
```

**INPUTMUX（DMA0チャンネルNのトリガーをCTimer2 Match0に）:**
```c
// MCUXpresso SDK の fsl_inputmux_connections.h で
// kINPUTMUX_Ctimer2M0ToDma0 の実際の値を確認してから使用
#define _DMA_CH  0U  // 使用するDMAチャンネル（Zephyrとの競合を確認）
INPUTMUX->DMA0_ITRIG_INMUX[_DMA_CH] = kINPUTMUX_Ctimer2M0ToDma0;
```

**DMA0 ディスクリプタ（ピンポン）:**
```c
// XFERCFG: WIDTH=2(32bit), SRCINC=1(+4), DSTINC=0(固定),
//          XFERCOUNT=255, RELOAD=1, CLRTRIG=1
// SRCBASEADDR: _vbuf[0] / _vbuf[1]
// DSTBASEADDR: 0x40105040（GPIO3->PDOR）
```

**先頭ピクセルのソフトウェアトリガー:**
```c
// DMAのSETTRIGビット（チャンネルのCONFIG レジスタ）を書いて即時1発目実行
DMA0->CHANNEL[_DMA_CH].XFERCFG |= (1U << 2);  // SWTRIG
// 以後はCTimer2 Match0がトリガー
```

### prj.conf への追加

```
CONFIG_DMA=y
```

### frdm_mcxa153.overlay への追加

```dts
&ctimer2 {
    status = "okay";
    /* PR/MR0 は video_on() で直接設定 */
};
```

### video_off() への追加

```c
// CTimer2停止
_CTIMER2_TCR = 0U;
// DMAチャンネル無効化
DMA0->CHANNEL[_DMA_CH].CFG &= ~(1U << 0);
```

---

## 重要な注意事項（実装時に参照）

### DMAトリガー設定（NXP Community確認済み）
- `kDMA_HighLevelTrigger` + `kDMA_SingleTransfer` の組み合わせのみ正しく動作
- `kDMA_RisingEdgeTrigger` や `PERIPHREQEN` 有効では動かない

### INPUTMUX値の確認が必要
MCUXpresso SDKの `fsl_inputmux_connections.h` で
`kINPUTMUX_Ctimer2M0ToDma0` の実際の数値を確認してから使う。

### DMAチャンネル番号
Zephyrが内部で使用しているDMAチャンネルと競合しないよう確認が必要。
MCXA153のDMA0は32チャンネル。高番号（ch28〜31）を使うと競合しにくい。

### PDOR書き込みと他ピンの干渉
GPIO3->PDORを32bit全体で書くため OUT2/OUT4/OUT5/OUT6 も含まれる。
映像期間中のOUT変更は1フレーム遅れで反映される（M6と同じ挙動）。
`pdor_base` = GPIO3->PDOR & ~(sync_mask | video_mask) をISR内で毎ライン取得する。

### video_off → video_on 再起動
`video_on()` でCTimer2・DMAを毎回初期化する（状態を確実にリセット）。
`_cvbs_cy_phase` の再測定も `counter_start()` 後に行われるため正しく動作する。

---

## ビルド・フラッシュ

```zsh
west build -b frdm_mcxa153
west flash
```

---

## 参照: 基本方針

- ハードウェアアクセスは Zephyr API を通す（APIがない場合は直叩き可、理由をコメント必須）
- 直叩きは `#if defined(CONFIG_SOC_SERIES_MCXA1X3)` で厳密にスコープを閉じる
- `IchigoJam_BASIC/` を変更しない
