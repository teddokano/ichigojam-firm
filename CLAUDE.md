# IchigoJam_Z — Zephyr移植プロジェクト

IchigoJam BASIC を Zephyr RTOS 上で動かすプロジェクト。
このリポジトリは `IchigoJam/ichigojam-firm` のフォーク。

---

## 基本方針

- **実装はできるだけシンプルに**
- **ハードウェアアクセスは必ずZephyr APIを通す**（レジスタ直叩き禁止）
- `IchigoJam_BASIC/` は変更しない（唯一の例外は下記1行のみ）
- **Claude Code は `IchigoJam_BASIC/` を自律的に変更してはならない**

---

## 現在の状態

**M0〜M4 完了。Pico（rpi_pico）と FRDM-MCXA153 の両ボードで動作確認済み。**
**作業ブランチ: `zephyr-m4`（M5はここから開始する）**

| | 内容 | 状態 |
|---|---|---|
| M0 | プロジェクト骨格 | 完了 |
| M1 | UART I/O | 完了 |
| M2 | GPIO + NVS + @ARUN自動起動 | 完了 |
| M3 | PWM/PSG + WAIT | 完了 |
| M4 | I2C + USR() | 完了 |
| M5 | FRDM-MCXC444 ポート | 未着手 |
| M6 | CVBSビデオ出力（Pico先行） | 未着手 |
| M7 | FRDM-MCXC444 + CVBS | 未着手 |

---

## ディレクトリ構成

```
ichigojam-firm/
├── CLAUDE.md
├── IchigoJam_BASIC/             ← BASICコア（ichigojam-stddef.hに1行追加のみ）
├── IchigoJam_P/                 ← 移植の参照実装（Pico SDK版）
├── console/                     ← 参照（各HAL関数のインターフェース定義）
└── IchigoJam_Z/
    ├── CMakeLists.txt
    ├── prj.conf                  ← 全ボード共通設定
    ├── boards/
    │   ├── rpi_pico.overlay
    │   ├── frdm_mcxa153.overlay
    │   └── frdm_mcxc444.overlay  ← M5で新規作成
    └── src/
        ├── main.c
        ├── config.h
        ├── display.h
        ├── flashstorage.h
        ├── ichigojam-io.h
        ├── keyboard.h
        ├── sound.h
        ├── system.h
        └── usr.h
```

ボード固有のKconfigが必要になった場合のみ `boards/<board>.conf` を追加する。

---

## 移植性のための設計原則

新しいボードへの移植は **overlayファイルの追加だけ** で完結するべき。
`.h`ファイル（HAL実装）にボード固有のデバイス名・チャンネル番号を書かない。

### ボード固有知識はすべてoverlayに閉じ込める

`zephyr,user`ノードや`chosen`にIchigoJam用のペリフェラル仕様を定義し、
HAL側はそこだけを参照する。新しいボードを追加する際は：
1. `boards/<board>.overlay` を新規作成してデバイスを指定するだけ
2. `.h`ファイルは変更しない

**音源PWM（現状は`.h`内に`#elif`が3段）→ overlayで解決：**
```dts
// rpi_pico.overlay  (channel=4はslice2chA、周期はダミー値)
/ { zephyr,user { ij-sound-pwms = <&pwm 4 1000 PWM_POLARITY_NORMAL>; }; };
// frdm_mcxa153.overlay
/ { zephyr,user { ij-sound-pwms = <&ctimer0 0 1000 PWM_POLARITY_NORMAL>; }; };
```
```c
// sound.h — ボード名が消える
static const struct pwm_dt_spec _snd =
    PWM_DT_SPEC_GET(DT_PATH(zephyr_user), ij_sound_pwms);
```

**ADC（現状は`adc` vs `lpadc0`の`#elif`）→ chosenで解決：**
```dts
// rpi_pico.overlay
/ { chosen { ij-adc = <&adc>; }; };
// frdm_mcxa153.overlay
/ { chosen { ij-adc = <&lpadc0>; }; };
```
```c
// ichigojam-io.h — ボード名が消える
#define _IJ_ADC DEVICE_DT_GET(DT_CHOSEN(ij_adc))
```

**I2C（現状は`i2c0` vs `lpi2c0`の`#elif`）→ chosenで解決：**
```dts
/ { chosen { ij-i2c = <&i2c0>; }; };   // または &lpi2c0
```

**OUT1〜6のIJB_pwm（現状はRP2040とFlexPWMで実装が2系統）→ `pwms`プロパティで解決：**
```dts
// rpi_pico.overlay
/ { zephyr,user {
    ij-pwm1-pwms = <&pwm 8 20000000 PWM_POLARITY_NORMAL>;  /* GPIO8 */
    ij-pwm2-pwms = <&pwm 9 20000000 PWM_POLARITY_NORMAL>;
    /* ... */
}; };
// frdm_mcxa153.overlay
/ { zephyr,user {
    ij-pwm1-pwms = <&flexpwm0_pwm0 0 20000000 PWM_POLARITY_NORMAL>;
    /* ... */
}; };
```
```c
// ichigojam-io.h — RP2040/FlexPWM/CTimerを区別せず同一コードで動く
static const struct pwm_dt_spec _pwm_out[] = {
    PWM_DT_SPEC_GET(DT_PATH(zephyr_user), ij_pwm1_pwms),
    PWM_DT_SPEC_GET(DT_PATH(zephyr_user), ij_pwm2_pwms),
    /* ... */
};
```

### 判断基準
- `.h`ファイルに `DT_NODELABEL(lpadc0)` `flexpwm0` 等のボード固有ラベルが
  現れたら設計を見直す
- `#elif` でボードを列挙し始めたらoverlayへの移動を検討する
- RP2040 PWM pinmux の `IO_BANK0` 直叩きは例外（ZephyrにAPIがないため）

---

## コア変更（唯一）

`IchigoJam_BASIC/ichigojam-stddef.h` に1行追加済み：
```c
#define PLATFORM_RP2040_ZEPHYR 11
```
アップストリームへの `git merge` で衝突するのはこの1行だけ。

---

## ピンアサイン（Raspberry Pi Pico）

| 機能 | GPIO | 備考 |
|---|---|---|
| UART TX/RX | GPIO0/1 | UART0 |
| SOUND | GPIO20 | PWM slice2 chA |
| OUT1〜OUT4 | GPIO8〜11 | |
| OUT5 | GPIO22 | |
| OUT6 | GPIO21 | |
| IN1 | GPIO27 | ADC ch1 |
| IN2 | GPIO26 | ADC ch0 |
| IN3/IN4 | GPIO6/7 | |
| LED | GPIO25 | ACTIVE_HIGH |
| BTN | GPIO28 | ACTIVE_LOW、ADC ch2 |
| I2C SDA/SCL | GPIO4/5 | I2C0 |
| CVBS SYNC | GPIO16 | M6で追加 |
| CVBS VIDEO | GPIO17 | M6で追加 |

## ピンアサイン（FRDM-MCXA153）

overlayの通り。OUT1=D2(P2_4), OUT2=D3(P3_0), OUT3=D4(P2_5),
OUT4=D5(P3_12, RED LED兼用), OUT5=D6(P3_13), OUT6=D7(P3_1),
IN1〜IN4=A0〜A3, SOUND=D13(P2_12, CTimer0), BTN=SW2(P3_29)。

---

## 既知の未実装・要修正箇所

### 1. `frames` と `_g.linecnt` のインクリメント漏れ
`sound.h` の `_sound_timer_fn` に追加が必要：
```c
psg_tick();
frames++;        // ← 追加
_g.linecnt++;    // ← 追加
```
これがないと `TICK(0)` が常に0、`WAIT -n`（フレーム待ち）が機能しない。

### 2. `IJB_wait()` の負値処理が未実装
```c
int IJB_wait(int n, int active) {
    if (n < 0) {
        _g.linecnt = 0;
        n = -n;
        while (_g.linecnt < (uint16_t)n) {
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
```

### 3. `system_init()` の実装
```c
void system_init(void) {
    _g.screen_big = 0;
    _g.screen_invert = 0;
    _g.lastfile = 0;
    _g.screen_insertmode = 1;
    key_flg.insert = 0;
    // noresmode は key_flg.keyflg_noresmode のマクロ。
    // key_flg はゼロ初期化されるので明示的な初期化不要。
}
```

### 4. RP2040 の PWM pinmux（レジスタ直叩き・残留確定）
`ichigojam-io.h` で `IO_BANK0 GPIO_CTRL[n].FUNCSEL` を直接書いている。
ZephyrのPinctrl APIはDTSで**静的**にpin-functionを定義するものであり、
runtime動的切り替えのAPIは現状Zephyrに存在しない。
このためレジスタ直叩きは**唯一の方法として残留確定**。
ただし以下を守ること：
- `#if defined(CONFIG_SOC_SERIES_RP2040)` で確実にスコープを閉じる
- 「ZephyrにAPIがないため直叩きを使う」旨をコメントで明記する

### 5. `IJB_reset()` が CMSIS の `NVIC_SystemReset()` を使っている
Zephyrの正式APIに置き換える：
```c
#include <zephyr/sys/reboot.h>
S_INLINE void IJB_reset(void) {
    sys_reboot(SYS_REBOOT_COLD);
}
```

### 6. MCXA153の `storage_partition` がNVSに対して小さすぎる可能性
MCXA153のflash sector size = **8KB**（`FSL_FEATURE_SYSCON_FLASH_SECTOR_SIZE_BYTES`）。
base DTSの storage_partition = **16KB** = **2 sectors** のみ。
`N_FLASH_STORAGE=100` ファイル分のデータには全く足りない。

対策（M5-a 開始時に要判断）：
- `N_FLASH_STORAGE` を現実的な値（例: 8〜16）に減らす、または
- `frdm_mcxa153.overlay` で storage_partition を拡大する

```dts
/* frdm_mcxa153.overlay で storage_partition を拡大する例（32KB = 4 sectors）*/
&flash0 {
    partitions {
        storage_partition: partition@18000 {
            label = "storage";
            reg = <0x18000 DT_SIZE_K(32)>;
        };
    };
};
```

### 7. `@ARUN` 自動起動の確認
`main.c` の `#if VER_PLATFORM == PLATFORM_LPC1114` には入らないため、
HAL側（`getSleepFlag()` またはその後）で対処している想定。
M5開始前に `@ARUN` プログラムを保存してリセット後に動作確認すること。

### 8. `displaymode` 変数（未使用・残留確定）
`keyboard.h` で `uint8 displaymode;` を定義しているが Zephyr版では参照なし。
BASICコアから要求される定義のため削除不可。コメントを明記しておく：
```c
uint8 displaymode;  // required by IchigoJam_BASIC core; unused in Zephyr port
```

### 9. `video_waitSync(1)` はM6でも16ms yieldを維持すること
`main.c` のメインループに `video_waitSync(1)` があり、コメントに
「消すとUART受信漏れ発生?」とある。
現状は `k_sleep(K_MSEC(16))` として実装されており、
`K_PRIO_PREEMPT(7)` のメインスレッドが定期的にCPUを手放すことで
UART IRQ処理の機会を確保している。
**M6でCVBSを実装して `video_waitSync()` の実装が変わる際も、
最低16msのyieldを保証すること。**

### 10. 小RAMボード移植時の注意（MCXC444など）
`basic.h` の `pchar` は `MEM_UNDER64KB` 未定義のため `char*`（4バイト）。
`gosubstack[30] + forstack[6]` で144バイト消費。
`ram[SIZE_RAM]`（約4.3KB）がBSSに確保される。
RAMが8KB以下のボードへの移植時は `MEM_UNDER64KB` の定義を検討する。
`main.c` の `#if VER_PLATFORM == PLATFORM_LPC1114` には入らないため、
HAL側（`getSleepFlag()` またはその後）で対処している想定。
M5開始前に `@ARUN` プログラムを保存してリセット後に動作確認すること。

---

## 実装スコープ

### 完了（M0〜M4）
- UART I/O（IRQ driven + msgq）
- GPIO IN/OUT/LED/BTN/ANA（DTS overlay でボード非依存）
- PWM / PSG音源（`BEEP`, `PLAY`）
- Flash ストレージ（`SAVE`, `LOAD`）
- I2C（`I2C()` コマンド）
- `USR()` マシン語呼び出し

### 今後（M5〜M7）
- M5: FRDM-MCXC444 ポート
- M6: CVBSビデオ出力（Pico先行）
- M7: FRDM-MCXC444 + CVBS

### スコープ外（恒久）
- USBキーボード
- DVI/HDMI 出力
- WS2812B LED
- `VERSION15` の追加コマンド

---

## ブランチ戦略

```
zephyr-m4   ← 現在地（M5-a の起点）
zephyr-m5a  ← M5-a 完了時にコミット（リファクタリング）
zephyr-m5   ← M5-b 完了時にコミット（MCXC444ポート）
zephyr-m6   ← M6 完了時にコミット（CVBSビデオ）
```

**Claude Code は `main` へのマージを自律的に行ってはならない。**

---

## 作業対象ファイル

**変更してよいのは `IchigoJam_Z/` 以下のみ。**

| ディレクトリ | 扱い |
|---|---|
| `IchigoJam_Z/` | 作業対象。自由に変更してよい |
| `IchigoJam_BASIC/` | **読まない・変更しない**（唯一の例外は `ichigojam-stddef.h` の既存1行のみ） |
| `IchigoJam_P/` | 参照のみ（移植の参考） |
| `IchigoJam_MCX/` | 参照のみ |
| `console/` | 参照のみ（HALインターフェース確認用） |

---

## Claude Code作業指針（トークン節約）

### 指示の出し方
- **1タスク1指示**に絞る。複数変更を一度に頼まない
- 変更対象ファイルと変更内容をCLAUDE.mdに明示しておく
- 例：「`sound.h`の`SOUND_PWM_NODE`検出を`pwm_dt_spec`に置き換える」

### ファイル読み込みの抑制
Claude Codeは変更前に関連ファイルを広く読む傾向がある。
CLAUDE.mdに「どのファイルを変更するか」「変更後のコードの骨格」を
具体的に書いておくと余分な探索を防げる。

### `hardware_map.yaml` の扱い
実機固有のシリアルIDとデバイスパスが書かれている。
`.gitignore` に追加するか `hardware_map.yaml.example` に変名する。

### `testcase.yaml` の修正（M5-b時に対応）
現状は `frdm_mcxa153` 専用でregexもボード名固定。
`platform_allow` を複数ボード対応にし、regexからボード名を外す：
```yaml
regex:
  - "IchigoJam BASIC 1\\.4 Zephyr"   # ボード名を含めない
platform_allow:
  - frdm_mcxa153
  - rpi_pico
  - frdm_mcxc444
```

---

## M5-a 開始時に最初にやること（順番通りに）

1. **`flashstorage.h` のincludeパスはそのまま**（修正不要・確認済み）
   - Zephyr 4.4 では `zephyr/kvss/nvs.h` が正しいパス（M5-a で確認済み）
   - `zephyr/fs/nvs.h` は deprecated で逆に警告が出る
   - CLAUDE.md の記述が誤りだった（旧バージョンのZephyr向け情報）

2. **MCXA153の `storage_partition` サイズ確認**
   MCXA153の flash sector size = 8KB。base DTSの storage_partition = 16KB = 2 sectors。
   `N_FLASH_STORAGE=100` には不足。overlay で拡大するか値を減らすか決める。
   （詳細は「既知の未実装・要修正箇所 6」を参照）

3. **`@ARUN` 動作確認**
   `@ARUN` プログラムを保存してリセット後に自動起動するか確認。
   未確認のまま進むと後で原因切り分けが困難になる。

4. **`frames` と `_g.linecnt` のインクリメント追加**（`sound.h`）
   ```c
   // _sound_timer_fn 内、psg_tick() の後
   frames++;
   _g.linecnt++;
   ```

5. **`IJB_wait()` の負値処理追加**（`system.h`）
   「既知の未実装・要修正箇所」の実装を追加する。

6. **`system_init()` の実装**（`system.h`）
   「既知の未実装・要修正箇所」の実装を追加する。

7. **`IJB_reset()` の置き換え**（`system.h`）
   `NVIC_SystemReset()` → `sys_reboot(SYS_REBOOT_COLD)`

8. **ADC・sound・I2C・IJB_pwm のリファクタリング**
   「移植性のための設計原則」に従い、各ファイルを1つずつ変更する。
   変更のたびにPicoとMCXA153でビルド確認する。

---

## M5: FRDM-MCXC444 ポート

### M5-a: リファクタリング（先行）

`.h`ファイルからボード固有ラベルを除去し、ボード知識をoverlayに移す。
**変更後にPicoとMCXA153で動作確認してから M5-b に進む。**

#### ADC: `chosen` で統一
```dts
/* rpi_pico.overlay に追加 */
/ { chosen { ij-adc = <&adc>; }; };
/* frdm_mcxa153.overlay に追加 */
/ { chosen { ij-adc = <&lpadc0>; }; };
```
```c
/* ichigojam-io.h: DT_NODELABEL(adc)/DT_NODELABEL(lpadc0) の #elif を削除 */
#define _IJ_ADC DEVICE_DT_GET(DT_CHOSEN(ij_adc))
```

#### 音源PWM: `pwm_dt_spec` で統一
```dts
/* rpi_pico.overlay の zephyr,user に追加 */
ij-sound-pwms = <&pwm 4 1000 PWM_POLARITY_NORMAL>;
/* frdm_mcxa153.overlay の zephyr,user に追加 */
ij-sound-pwms = <&ctimer0 0 1000 PWM_POLARITY_NORMAL>;
```
```c
/* sound.h: DT_NODELABEL(pwm)/#elif ctimer0/#elif flexpwm の3段を削除 */
static const struct pwm_dt_spec _snd =
    PWM_DT_SPEC_GET(DT_PATH(zephyr_user), ij_sound_pwms);
```

#### I2C: `chosen` で統一
```dts
/* rpi_pico.overlay に追加 */
/ { chosen { ij-i2c = <&i2c0>; }; };
/* frdm_mcxa153.overlay に追加 */
/ { chosen { ij-i2c = <&lpi2c0>; }; };
```
```c
/* ichigojam-io.h: i2c0/lpi2c0 の #elif を削除 */
#define _IJ_I2C_DEV DEVICE_DT_GET(DT_CHOSEN(ij_i2c))
```

#### IJB_pwm（OUT1〜6）: `pwm_dt_spec` で統一
```dts
/* rpi_pico.overlay の zephyr,user に追加 */
ij-pwm1-pwms = <&pwm 8 20000000 PWM_POLARITY_NORMAL>;
ij-pwm2-pwms = <&pwm 9 20000000 PWM_POLARITY_NORMAL>;
/* ... OUT3〜6も同様 */
/* frdm_mcxa153.overlay の zephyr,user に追加 */
ij-pwm1-pwms = <&flexpwm0_pwm0 0 20000000 PWM_POLARITY_NORMAL>;
/* ... */
```
```c
/* ichigojam-io.h: RP2040とFlexPWMの2系統実装をループ1本に統一 */
static const struct pwm_dt_spec _pwm_out[] = {
    PWM_DT_SPEC_GET(DT_PATH(zephyr_user), ij_pwm1_pwms),
    PWM_DT_SPEC_GET(DT_PATH(zephyr_user), ij_pwm2_pwms),
    /* ... */
};
```

**注意: RP2040のFUNCSEL直叩きは `pwm_dt_spec` 化後も残る。**
`pwm_dt_spec` はデバイス+チャンネル+周期の抽象化であり、
GPIO→PWMのpinmux切り替えは含まない。
`pwm_set()` の前後に `IO_BANK0 GPIO_CTRL` 操作が引き続き必要。
`#if defined(CONFIG_SOC_SERIES_RP2040)` で閉じた上で理由をコメント明記。

#### ADC chosenへの移行時の注意
`chosen { ij-adc }` でデバイスだけを渡す。
チャンネルごとの `gain`/`reference`/`input_positive` 設定は
引き続きoverlayの `channel@N` ノードで定義する（現状維持）。
`_adc_read()` 内の `lpadc0` 固有チャンネルマッピング（`mcxa_hw_ch[]`）は
overlayの `channel@N` 定義が正しければ不要になる可能性がある—要確認。

---

### M5-b: FRDM-MCXC444 ポート

M5-a 完了後、`boards/frdm_mcxc444.overlay` を新規作成するだけで動くはず。
`.h`ファイルは変更しない。

MCXC444 固有の注意点：
- FlexPWM なし → 音源は `ctimer0` の PWM ドライバを `ij-sound-pwms` に指定
- IJB_pwm は CTimer または TPM を `ij-pwm1-pwms` 等に指定
- ADC チャンネルは `chosen { ij-adc = <&...>; }` に指定

```zsh
west build -b frdm_mcxc444
```

---

## M6: CVBSビデオ出力

### 前回（zephyr-m4-video）の失敗原因
レジスタ直叩き・fsl_clock.h・SPI・ISR+セマフォ+スレッドの複合構成。
M6はZephyr APIのみで実装し直す。

### 回路
```
GPIO(SYNC) ─── 470Ω ──┬── CVBS出力（75Ω終端のテレビへ）
GPIO(VIDEO) ── 100Ω ───┘
```

| SYNC | VIDEO | 出力電圧 | 意味 |
|---|---|---|---|
| L | L | 0V | 同期チップ |
| H | L | 約0.38V | ブランク/黒 |
| H | H | 約0.97V | 白 |

回路図: https://ichigojam.net/data/IchigoJam-T-howtobuild.pdf

### タイミング（NTSC）
- 1ライン = 63.5μs、261ライン/フレーム、約60fps
- フレーム構成: Vsync 3ライン + ブランク 19ライン + 有効 192ライン + ブランク 47ライン

### 実装方針：Zephyr `counter` API

PicoもMCXA153も `counter_set_channel_alarm()` 対応確認済み：
- Pico: `timer0`（RP2040 hardware timer）
- MCXA153: `ctimer1`（`nxp,lpc-ctimer` compatible）

```c
// display.h の骨格
static void line_alarm_cb(const struct device *dev,
                          uint8_t chan, uint32_t ticks, void *ud)
{
    // SYNCをLOW → 次アラームをセット → SYNCをHIGH → VIDEOピクセル出力
    // frames / _g.linecnt インクリメント（timer_threadから移管）
    counter_set_channel_alarm(dev, 0, &_alarm);
}

void video_on(void) {
    SCREEN_W = 32; SCREEN_H = 24;
    // counter_us_to_ticks() で63.5μsをtick値に変換
    // counter_start() → counter_set_channel_alarm() で開始
}
```

SPIもスレッドも不要。`CONFIG_COUNTER=y` を `prj.conf` に追加するだけ。

### M6開始前のステップ
1. `west build -b rpi_pico samples/drivers/counter/alarm` でcounter動作確認
2. コールバック内でGPIO操作しオシロで63.5μs周期を確認
3. 確認できたら `display.h` に本実装を追加

---

## ビルドコマンド

```zsh
west build -b rpi_pico          # Pico
west build -b frdm_mcxa153      # FRDM-MCXA153
west build -b frdm_mcxc444      # FRDM-MCXC444（M5以降）
```

## フラッシュ方法

**Pico（SWDなし）**: BOOTSELボタンを押しながらUSB接続 →
`build/zephyr/zephyr.uf2` をマスストレージにコピー

**Pico（SWDあり）**: `west flash --openocd /usr/local/bin/openocd`

**FRDM系**: `west flash`
