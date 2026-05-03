# IchigoJam_Z — Zephyr移植プロジェクト

IchigoJam BASIC を Zephyr RTOS 上で動かすプロジェクト。
このリポジトリは `IchigoJam/ichigojam-firm` のフォーク。

---

## ディレクトリ構成

```
ichigojam-firm/
├── CLAUDE.md                   ← このファイル
├── IchigoJam_BASIC/            ← BASICコア（変更しない）
├── IchigoJam_P/                ← 移植の参照実装（Pico SDK版）
├── IchigoJam_MCX/              ← 参照（GPIO/Soundはスタブ）
├── console/                    ← 参照（各HAL関数のインターフェース定義）
└── IchigoJam_Z/                ← 今回の移植先（新規作成）
    ├── CMakeLists.txt
    ├── prj.conf
    ├── rpi_pico.overlay
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

---

## 設計方針

### ベース実装
- **コア**: `IchigoJam_BASIC/` をそのまま使用（ただし1点だけ変更あり、下記参照）
- **HAL実装の参照**: `IchigoJam_P/src/` の各ヘッダを Zephyr API に読み替える
- **インターフェース仕様の参照**: `console/src/` の各ヘッダ（関数シグネチャの正解）
- **BASICバージョン**: 1.4ベース（`VERSION15` は使わない）
- `VER_PLATFORM` は `PLATFORM_RP2040_ZEPHYR`（11）を使用

### `ichigojam-stddef.h` への追加（唯一のコア変更）
`IchigoJam_BASIC/ichigojam-stddef.h` に1行追加する：
```c
#define PLATFORM_RP2040_ZEPHYR 11
```
これにより将来 `#if VER_PLATFORM == PLATFORM_RP2040_ZEPHYR` で
Zephyr固有の分岐が書けるようになる。

### スレッド構成

| スレッド | 優先度 | 役割 |
|---|---|---|
| `uart_rx_thread` | `K_PRIO_COOP(2)` | UART割り込み → `k_msgq` → `key_getKey()` |
| `timer_thread` | `K_PRIO_COOP(1)` | 16666μs周期 → `psg_tick()` + `set_tone()` |
| `basic_thread` | `K_PRIO_PREEMPT(7)` | BASICインタープリタ本体 |

### ピンアサイン（Raspberry Pi Pico）

| 機能 | GPIO | 備考 |
|---|---|---|
| UART TX | GPIO0 | UART0 |
| UART RX | GPIO1 | UART0 |
| SOUND | GPIO20 | PWM2A |
| OUT1〜OUT4 | GPIO8〜11 | |
| OUT5 | GPIO22 | |
| OUT6 | GPIO21 | |
| IN1 | GPIO27 | ADC兼用 |
| IN2 | GPIO26 | ADC兼用 |
| IN3 | GPIO6 | |
| IN4 | GPIO7 | |
| LED | GPIO25 | オンボードLED |
| BTN | GPIO28 | ADC兼用 |
| I2C SDA | GPIO4 | I2C0 |
| I2C SCL | GPIO5 | I2C0 |

### HAL関数の実装対応表

| `IchigoJam_P` (Pico SDK) | `IchigoJam_Z` (Zephyr) |
|---|---|
| `gpio_put(pin, v)` | `gpio_pin_set_dt(&pin, v)` |
| `gpio_get(pin)` | `gpio_pin_get_dt(&pin)` |
| `pwm_set_wrap/level()` | `pwm_set(dev, ch, period, pulse)` |
| `uart_putc(c)` | `uart_poll_out(uart_dev, c)` |
| `uart_irq_callback` | `uart_irq_callback_set()` → `k_msgq_put()` |
| `key_pushc(ch)` → `key_getKey()` | `k_msgq_put()` → `k_msgq_get()` |
| `add_repeating_timer_us(-16666, ...)` | `k_timer` + `timer_thread` |
| `flash_range_erase/program()` | `nvs_write()` / `nvs_read()` |
| `time_us_64()` | `k_cyc_to_us_floor64(k_cycle_get_32())` |
| `sleep_ms(n)` | `k_sleep(K_MSEC(n))` |

---

## 実装スコープ

### 対象（M0〜M4）
- UART シリアル入出力
- GPIO IN/OUT/LED/BTN
- PWM / PSG音源（`BEEP`, `PLAY`）
- Flash ストレージ（`SAVE`, `LOAD`）
- I2C（`I2C()` コマンド）
- `USR()` マシン語呼び出し（M4、MPU無効前提で確認後に対応）

### スコープ外
- USBキーボード（TinyUSB HID host）
- DVI/HDMI ビデオ出力（シリアルターミナルのみ）
- WS2812B LED（`WS.LED`）
- `VERSION15` の追加コマンド（`DAC`, `KBD` 等）

---

## 重要な実装上の注意

### `key_getKey()` の実装
`k_msgq_get()` は `K_NO_WAIT` でポーリングする。
入力なしの場合は -1 を返し、BASICのメインループが `continue` する。
CPUを食い続けないよう `K_MSEC(1)` のスリープを入れる。

```c
// keyboard.h
int key_getKey(void) {
    char ch;
    if (k_msgq_get(&uart_msgq, &ch, K_NO_WAIT) == 0) {
        return (int)(uint8_t)ch;
    }
    k_sleep(K_MSEC(1));
    return -1;
}
```

### `stopExecute()` の実装
`IJB_wait()` 内での ESC 検出は `k_sleep(K_MSEC(n))` ではなく
16ms刻みのループポーリングで実装する。

```c
// system.h
int IJB_wait(int n, int active) {
    int ticks = n; // 1tick = 1/60秒
    while (ticks > 0) {
        k_sleep(K_MSEC(16));  // 約1tick（60Hz）
        ticks--;
        if (stopExecute()) return 1;  // ESCで中断
    }
    return 0;
}
```

### `display.h`
console版と同じ実装でよい（シリアルターミナルに出力）。
`video_on()` で `SCREEN_W=32`, `SCREEN_H=24` をセットするだけ。

### `flashstorage.h` / NVS
- `CONFIG_NVS=y`, `CONFIG_FLASH=y`, `CONFIG_FLASH_MAP=y`
- namespace: `"ij"`, key = ファイル番号（0〜99）
- `N_FLASH_STORAGE 100`（P版と同じ）
- NVS の `storage_partition` は Pico の DTS に定義済み

### `usr.h`
P版と同じ実装（RAMアドレスを関数ポインタでCALL）。
RP2040 は `CONFIG_ARM_MPU` がデフォルト無効なのでそのまま動く見込み。
M4で実機確認後に判断。

### `ext_iot.h`
`IchigoJam_BASIC/ext_iot.h` を使う。
`config.h` で `#define EXT_IOT` を定義する。

---

## マイルストーン

| | 内容 | 検証方法 |
|---|---|---|
| M0 | プロジェクト骨格 | `west build -b rpi_pico` が通る |
| M1 | UART I/O | `PRINT "HELLO"` → シリアルに表示 |
| M2 | GPIO + NVS | `OUT1,1` でLED点灯。`SAVE0` → リセット → `LOAD0` |
| M3 | PWM/PSG + WAIT | `BEEP` で音が出る。`WAIT 60` が約1秒 |
| M4 | I2C + USR() | I2Cデバイス疎通。`USR()` でマシン語実行 |

## フラッシュ方法
SWDデバッガなしの場合：BOOTSELボタンを押しながらUSB接続し、
`build/zephyr/zephyr.uf2` をマスストレージにコピー。

SWDデバッガ（Debug Probe等）がある場合：
```
west flash --openocd /usr/local/bin/openocd
```

---

## ビルドコマンド
```zsh
cd IchigoJam_Z
west build -b rpi_pico -- -DBOARD_ROOT=.
# または
west build -b rpi_pico
```
