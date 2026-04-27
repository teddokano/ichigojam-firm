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

**M0〜M6 完了。Pico（rpi_pico）で CVBS テレビ出力の実機動作確認済み。**
**作業ブランチ: `zephyr-m5`（M7 はここから継続）**

| | 内容 | 状態 |
|---|---|---|
| M0 | プロジェクト骨格 | 完了 |
| M1 | UART I/O | 完了 |
| M2 | GPIO + NVS + @ARUN自動起動 | 完了 |
| M3 | PWM/PSG + WAIT | 完了 |
| M4 | I2C + USR() | 完了 |
| M5a | HALリファクタリング（overlay統一） | 完了 |
| M5b | FRDM-MCXC444 ポート | 完了（実機確認済み） |
| **M5** | **3ボード実機確認（ANA/SAVE/LOAD/PWM/I2C）** | **完了** |
| **M6** | **CVBSビデオ出力（Pico先行）** | **完了（実機確認済み）** |
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

### 1. RP2040 の PWM pinmux（レジスタ直叩き・残留確定）
`ichigojam-io.h` で `IO_BANK0 GPIO_CTRL[n].FUNCSEL` を直接書いている。
ZephyrのPinctrl APIはDTSで**静的**にpin-functionを定義するものであり、
runtime動的切り替えのAPIは現状Zephyrに存在しない。
このためレジスタ直叩きは**唯一の方法として残留確定**。
ただし以下を守ること：
- `#if defined(CONFIG_SOC_SERIES_RP2040)` で確実にスコープを閉じる
- 「ZephyrにAPIがないため直叩きを使う」旨をコメントで明記する

### 2. MCXA153・MCXC444 の IN1-IN4 デジタル読み取り
`io_init()` で IN1-IN4 の `gpio_pin_configure_dt()` をスキップするため（ADCピンを
アナログモードのまま保持するため）、`IN()` の bit0-3 は常に 0 を返す。
MCXA153・MCXC444 では IN1-IN4 は ADC 専用ピン（ANA(1)～ANA(4) のみ使用可）。

### 4. `displaymode` 変数（未使用・残留確定）
`keyboard.h` で `uint8 displaymode;` を定義しているが Zephyr版では参照なし。
BASICコアから要求される定義のため削除不可。

### 5. `video_waitSync(1)` はM6でも16ms yieldを維持すること
現状は `k_sleep(K_MSEC(16))` として実装。
**M6でCVBSを実装して `video_waitSync()` の実装が変わる際も、
最低16msのyieldを保証すること。**

### 6. 小RAMボード移植時の注意（MCXC444など）
`basic.h` の `pchar` は `MEM_UNDER64KB` 未定義のため `char*`（4バイト）。
`gosubstack[30] + forstack[6]` で144バイト消費。
`ram[SIZE_RAM]`（約4.3KB）がBSSに確保される。
RAMが8KB以下のボードへの移植時は `MEM_UNDER64KB` の定義を検討する。

---

## 実装スコープ

### 完了（M0〜M6）
- UART I/O（IRQ driven + msgq）
- GPIO IN/OUT/LED/BTN/ANA（DTS overlay でボード非依存）
- PWM / PSG音源（`BEEP`, `PLAY`）
- Flash ストレージ（`SAVE`, `LOAD`）
- I2C（`I2C()` コマンド）
- `USR()` マシン語呼び出し
- 3ボード実機確認（rpi_pico / frdm_mcxa153 / frdm_mcxc444）
- CVBSビデオ出力（rpi_pico 実機確認済み）

### 今後（M7）
- M7: FRDM-MCXC444 + CVBS

### スコープ外（恒久）
- USBキーボード
- DVI/HDMI 出力
- WS2812B LED
- `VERSION15` の追加コマンド

---

## ブランチ戦略

```
zephyr-m5   ← 現在地（M6 完了・CVBS実機確認済み）
zephyr-m7   ← M7 完了時にコミット（FRDM-MCXC444 + CVBS）
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

---

## M5-a 完了サマリ（zephyr-m5 ブランチ）

以下がすべて実装・動作確認済み：

- `flashstorage.h`: `zephyr/kvss/nvs.h`（Zephyr 4.4 の正式パス）
- `frdm_mcxa153.overlay`: storage_partition を 32KB（4セクタ）に拡大
- `sound.h`: `frames++` / `_g.linecnt++` 追加、`pwm_dt_spec` で統一
- `system.h`: `system_init()` 実装、`IJB_wait()` 負値処理、`sys_reboot()` 置換
- `ichigojam-io.h`: ADC/I2C/PWM をすべて overlay 駆動に統一
- LPADC IN1-IN4 GPIO設定スキップ（ADCピンをアナログモードに保持）
- 動作確認: ANA()・I2C() 正常動作

### ⚠️ `/chosen` プロパティの記法

`DT_HAS_CHOSEN()` が機能するには **`&foo` 形式（`< >` なし）** で書く必要がある：

```dts
/* ✓ 正しい（Type.PATH として edtlib が解釈し DT_CHOSEN_xxx_EXISTS を生成） */
chosen {
    ij-adc = &lpadc0;
    ij-i2c = &lpi2c0;
};

/* ✗ 間違い（Type.PHANDLE → to_path() が DTError → マクロ未生成 → 機能しない） */
chosen {
    ij-adc = <&lpadc0>;
    ij-i2c = <&lpi2c0>;
};
```

---

## M5-b: FRDM-MCXC444 ポート（完了）

`boards/frdm_mcxc444.overlay` を新規作成。
`.h` の変更が2点必要だった（overlay-only には収まらなかった）。

### 実装内容
- `boards/frdm_mcxc444.overlay` 新規作成
  - TPM0(OUT1/OUT3/OUT4/OUT5/OUT6), TPM1(OUT2), TPM2(SOUND) のピンctrl + 有効化
  - ADC0: PTB0-3（SE8/SE9/SE12/SE13）= Arduino A0-A3 ヘッダ
  - `/chosen { ij-adc = &adc0; ij-i2c = &i2c1; }`（i2c1 = D18/D19）
  - `ij-out-tpm-pcr` で PORT_PCR アドレス/ピン/MUX値を overlay に記述
- `src/ichigojam-io.h` 修正
  1. Kinetis ADC16 チャンネル設定: `channel_id` = SE番号直接 (`{8,9,12,13}`)
  2. ADC リファレンス: `ADC_REF_VDD_1`（VALT=VDDA=3.3V）を使用
  3. PCR pinmux 復元: `ij-out-tpm-pcr` プロパティを `DT_PROP()` で読み取り
- `src/flashstorage.h` 修正
  - NVS sector_size を 2KB 以上に切り上げ（1KB erase block 対応）

### MCXC444 固有の制約
- ADC16: `channel_id` = SE番号直接（LPADC の `input_positive` とは異なる）
- **ADC リファレンス: `ADC_REF_VDD_1`（VALT=VDDA=3.3V）を使うこと**
  - `ADC_REF_INTERNAL`（REFSEL=0=VREFH）は使用不可
  - VREFH は VREF モジュール出力（~1.18V 精密リファレンス）に接続されているが、
    VREF モジュールはデフォルト disabled → VREFH が ~0.13V に漂流する
  - 結果: 0.13V 入力で ANA()=1023 になる（フルスケールが 0.13V になる）
  - 公式サンプル `samples/drivers/adc/adc_dt/boards/frdm_mcxc444.overlay` も `ADC_REF_VDD_1` を使用
- ADC ピン: IN1-IN4 = PTB0-3（SE8/SE9/SE12/SE13）= Arduino A0-A3 ヘッダ
  - BSP の pinmux_adc0 が PTE20/PTE22 を定義しているが、それは A0-A3 ではない
  - GND を Arduino A0 に接続して ANA(1)=0 で確認済み
- TPM0 共有: OUT1(CH3)/OUT3(CH1)/OUT4(CH2)/OUT5(CH2)/OUT6(CH3) は同一周期
- TPM1 独立: OUT2(CH0) のみ → 独自周期設定可
- TPM2 独立: SOUND(CH0) → BEEP/PLAY が OUT PWM 周期に干渉しない
- IN1-IN4 はアナログ専用（デジタル読み取り不可、MUX0=アナログモード保持のため）

### ピンアサイン（実機確認済み）
- 詳細: `IchigoJam_Z/pinmap/frdm_mcxc444/frdm_mcxc444.txt`

```zsh
west build -b frdm_mcxc444
```

---

## M5 完了サマリ（実機確認項目）

3ボードすべてで動作確認済み：

| 機能 | rpi_pico | frdm_mcxa153 | frdm_mcxc444 |
|---|---|---|---|
| UART（起動ロゴ） | ✓ | ✓ | ✓ |
| LED / BTN | ✓ | ✓ | ✓ |
| OUT1-6 / IN(0) | ✓ | ✓ | ✓ |
| ANA(1)-(4) | ✓ | ✓ | ✓ |
| BEEP / PLAY | ✓ | ✓ | ✓ |
| PWM 1-6 | ✓ | ✓ | ✓ |
| SAVE / LOAD | ✓ | ✓ | ✓ |
| I2C() | ✓ | ✓ | ✓ |

---

## M6: CVBSビデオ出力（完了・実機確認済み）

### 回路
```
GPIO16(SYNC) ─── 470Ω ──┬── CVBS出力（75Ω終端のテレビへ）
GPIO17(VIDEO)── 100Ω ───┘
```

| SYNC | VIDEO | 出力電圧 | 意味 |
|---|---|---|---|
| L | L | 0V | 同期チップ |
| H | L | 約0.38V | ブランク/黒 |
| H | H | 約0.97V | 白 |

回路図: https://ichigojam.net/data/IchigoJam-T-howtobuild.pdf

### タイミング（NTSC）
- 1ライン = 64µs（63.5µsに近似）、261ライン/フレーム、≈59.9fps
- フレーム構成: vsync(12) + vblank(9) + active(192) + vblank(48)
  - vsync 12ライン = IchigoJam_Q 仕様（58/64µs = 91% duty ロングパルス）

### 実装（`src/display.h` + `boards/rpi_pico.overlay`）

- `CONFIG_COUNTER=y` を `prj.conf` に追加
- `chosen { ij-cvbs-timer = &timer; }` + GPIO16/17 を `rpi_pico.overlay` に追加
- `display.h` にライン ISR `_cvbs_line_cb()` を実装
  - 先頭で次ライン 64µs アラームを再スケジュール（ジッタ補正付き）
  - `frames` / `_g.linecnt` インクリメント
  - SIO 直叩きで SYNC パルス（4µs）+ バックポーチ（4µs）を生成
  - アクティブライン: 32 キャラクタ列 × 8ビット/列 × ~125ns/ビット
    - 各ビットを VIDEO=H/L で出力し文字パターンを再現（判読可能）
    - TIMELR アンカーで列幅を 1µs に固定（branch timing ドリフト吸収）

### RP2040 レジスタ直叩き（例外として許容）

```c
/* SIO SET/CLR: Zephyr GPIO API (~200ns/call) はジッタが大きすぎるため直叩き */
#define _SIO_SET (*(volatile uint32_t *)0xD0000014U)
#define _SIO_CLR (*(volatile uint32_t *)0xD0000018U)

/* TIMELR: k_busy_wait() は ~0.36µs/call のオーバーヘッドで 64µs 予算を超過するため
 * 1MHz フリーランカウンタを直接読んで正確に待機する */
#define _CVBS_TIMER_RAW (*(volatile uint32_t *)0x4005400CU)
```

いずれも `#if defined(CONFIG_SOC_SERIES_RP2040)` でスコープを閉じ、コメントを明記。

### counter_cancel_channel_alarm の正式名称

`counter_cancel_alarm()` は存在しない。正しくは `counter_cancel_channel_alarm(dev, chan_id)`。

### -ETIME チェーン断絶防止

`counter_set_channel_alarm()` が `-ETIME` を返した場合、driver が callback を NULL に
クリアして ISR チェーンが断絶する。必ず戻り値を確認してリカバリすること（実装済み）。

### 実機確認済み（Toshiba テレビ）
- H-sync: 15.625 kHz ✓
- V-sync: 安定（ローリングなし）✓
- 映像: 「IchigoJam BASIC x.x.x by jig.jp」「OK」が画面上部に表示 ✓

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
