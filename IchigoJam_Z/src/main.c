#if 0
// IchigoJam_Z - Zephyr entry point

#include <zephyr/kernel.h>

// ichigojam_main() is defined in IchigoJam_BASIC/main.c
// (compiled as a separate translation unit with our HAL headers on the path)
void ichigojam_main(void);

int main(void)
{
    ichigojam_main();
    return 0;
}
#else


#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#if 0
#define GPIO3_BASE DT_REG_ADDR(DT_NODELABEL(gpio3))
#define GPIO3_PSOR (*(volatile uint32_t *)(GPIO3_BASE + 0x44))
#define GPIO3_PCOR (*(volatile uint32_t *)(GPIO3_BASE + 0x48))
#define GPIO3_PDDR (*(volatile uint32_t *)(GPIO3_BASE + 0x54))
#endif

#include <stdint.h>
#include <stdbool.h>

/* =========================
 *  SoC 固有ベースアドレス（要確認）
 * ========================= */

#if 1
#define GPIO3_BASE      0x40105000U
#define GPIO3_PSOR      (*(volatile uint32_t *)(GPIO3_BASE + 0x44))
#define GPIO3_PCOR      (*(volatile uint32_t *)(GPIO3_BASE + 0x48))
#define GPIO3_PDDR      (*(volatile uint32_t *)(GPIO3_BASE + 0x54))
#endif

#define CTIMER1_BASE    0x40005000U
#define CTIMER1_IR      (*(volatile uint32_t *)(CTIMER1_BASE + 0x00))
#define CTIMER1_TC      (*(volatile uint32_t *)(CTIMER1_BASE + 0x08))
#define CTIMER1_PR      (*(volatile uint32_t *)(CTIMER1_BASE + 0x0C))
#define CTIMER1_MCR     (*(volatile uint32_t *)(CTIMER1_BASE + 0x14))
#define CTIMER1_MR0     (*(volatile uint32_t *)(CTIMER1_BASE + 0x18)) /* HSYNC/ライン */
#define CTIMER1_MR1     (*(volatile uint32_t *)(CTIMER1_BASE + 0x1C)) /* ピクセル */

#define DMA_BASE        0x40008000U
typedef struct {
	volatile uint32_t SRC;
	volatile uint32_t DST;
	volatile uint32_t CTRL;
	volatile uint32_t CFG;
} dma_ch_t;

/* チャネル割当（例） */
#define DMA_CH_SET   ((dma_ch_t *)(DMA_BASE + 0x100))
#define DMA_CH_CLR   ((dma_ch_t *)(DMA_BASE + 0x120))

/* DMAMUX（要機種確認） */
#define DMAMUX_BASE      0x40021000U
#define DMAMUX_CHCFG(n)  (*(volatile uint32_t *)(DMAMUX_BASE + 0x00 + (n)*4))

/* 例：CTIMER1 MR1 を DMA トリガにするID（要RMで確認して差し替え） */
#define DMAMUX_SRC_CTIMER1_MR1   (12U)   /* ★要調整 */
#define DMAMUX_ENBL              (1U << 31)

/* =========================
 *  ピン定義
 * ========================= */
#define SYNC_PIN   14
#define VIDEO_PIN  15
#define SYNC_MASK   (1U << SYNC_PIN)
#define VIDEO_MASK  (1U << VIDEO_PIN)

/* =========================
 *  NTSC 近似パラメータ
 * ========================= */
#define LINE_US      64U
#define HSYNC_US      4U
#define BACK_US       8U
#define VSYNC_LINES  12U
#define TOTAL_LINES 261U

/* ピクセル：96MHz想定で約22cy/px → 約0.23us/px */
#define PIX_CYCLES   22U
#define CPU_HZ       96000000U

/* =========================
 *  ピクセル（LUT廃止：動的生成）
 * ========================= */
typedef struct {
    uint32_t set;
    uint32_t clr;
} pix_t;

/* フォント（display側のものを使う想定でもOK） */
const uint8_t CHAR_PATTERN[256*8];

/* 1ライン：32文字×8px */
#define PIXELS_PER_LINE (32*8)
static pix_t line_buf[PIXELS_PER_LINE];

/* VRAM（例） */
//__attribute__((section(".ram_data")))
static uint8_t vram[32*24];

/* =========================
 *  ピクセル生成（1bit→GPIOパターン）
 * ========================= */
static inline pix_t make_pix(uint8_t bit)
{
    pix_t p;
    if (bit) {
        p.set = VIDEO_MASK;
        p.clr = 0;
    } else {
        p.set = 0;
        p.clr = VIDEO_MASK;
    }
    return p;
}

/* =========================
 *  ライン展開
 * ========================= */
static void build_line_pixels(uint8_t *row, int char_row)
{
	int idx = 0;
	for (int col = 0; col < 32; col++) {
		uint8_t ch = row[col];
        uint8_t pat = CHAR_PATTERN[ch*8 + char_row];
        for (int b = 0; b < 8; b++) {
            uint8_t bit = (pat >> (7 - b)) & 1;
            line_buf[idx++] = make_pix(bit);
        }	}
}

/* =========================
 *  DMA初期化
 * ========================= */
static void dma_init(void)
{
	/* SET */
	DMA_CH_SET->SRC  = (uint32_t)&line_buf[0].set;
	DMA_CH_SET->DST  = (uint32_t)&GPIO3_PSOR;
	DMA_CH_SET->CTRL = (1U<<0) | (1U<<4); /* EN + SRC_INC, DST固定は0想定 */
	DMA_CH_SET->CFG  = 0;

	/* CLR */
	DMA_CH_CLR->SRC  = (uint32_t)&line_buf[0].clr;
	DMA_CH_CLR->DST  = (uint32_t)&GPIO3_PCOR;
	DMA_CH_CLR->CTRL = (1U<<0) | (1U<<4);
	DMA_CH_CLR->CFG  = 0;

	/* DMAMUX：MR1でトリガ */
	DMAMUX_CHCFG(0) = DMAMUX_ENBL | DMAMUX_SRC_CTIMER1_MR1; /* CH0→SET */
	DMAMUX_CHCFG(1) = DMAMUX_ENBL | DMAMUX_SRC_CTIMER1_MR1; /* CH1→CLR */
}

/* =========================
 *  CTimer初期化
 * ========================= */
static void ctimer_init(void)
{
	/* 96MHz → 1MHz (1us) */
	CTIMER1_PR = (CPU_HZ/1000000U) - 1U;

	/* MR0: ライン、MR1: ピクセル */
	CTIMER1_MCR = (1U<<0) | (1U<<3); /* MR0I, MR1I（割り込みは使わなくてもOK） */
}

/* =========================
 *  1ライン開始（同期＋ピクセル開始）
 * ========================= */
static uint32_t next_line_time;
static uint16_t line;

static void start_line(void)
{
	uint32_t t0 = next_line_time;

	/* --- SYNC=L --- */
	while ((int32_t)(CTIMER1_TC - t0) < 0) {}
	GPIO3_PCOR = SYNC_MASK | VIDEO_MASK;

	/* --- HSYNC終了 --- */
	uint32_t t_h = t0 + HSYNC_US;
	while ((int32_t)(CTIMER1_TC - t_h) < 0) {}
	GPIO3_PSOR = SYNC_MASK;

	/* --- BACK PORCH終了 → ピクセル開始 --- */
	uint32_t t_px = t0 + HSYNC_US + BACK_US;

	/* 表示行判定 */
	bool active = (line >= 33 && line < (33+192));
	int vln = (int)line - 33;
	int char_row = vln & 7;

	if (active) {
		build_line_pixels(&vram[(vln>>3)*32], char_row);

		/* DMA転送数設定（実機に合わせて要設定項目追加） */
		DMA_CH_SET->SRC = (uint32_t)&line_buf[0].set;
		DMA_CH_CLR->SRC = (uint32_t)&line_buf[0].clr;

		/* ピクセルクロック開始：MR1を一定周期で進める */
		uint32_t next = t_px;
		for (int i = 0; i < PIXELS_PER_LINE; i++) {
			/* MR1を次ピクセル時刻にセット → DMAMUXでDMA発火 */
			CTIMER1_MR1 = next;
			next += (PIX_CYCLES / (CPU_HZ/1000000U)); /* ≒0.23us刻み（要実機調整） */
			/* ここは“ハードトリガでDMAが動く”ので待つ必要はないが、
			   最小構成では安全のため軽い待ちを入れてもOK */
			while ((int32_t)(CTIMER1_TC - CTIMER1_MR1) < 0) {}
		}

		/* ライン終端で黒に戻す */
		GPIO3_PCOR = VIDEO_MASK;
	} else {
		/* 非表示：黒レベル */
		uint32_t t_b = t0 + HSYNC_US + BACK_US;
		while ((int32_t)(CTIMER1_TC - t_b) < 0) {}
		GPIO3_PSOR = VIDEO_MASK;
	}

	/* 次ラインへ */
	next_line_time += LINE_US;
	line = (line + 1) % TOTAL_LINES;
}

/* =========================
 *  メイン
 * ========================= */
int main(void)
{
#if 0
	GPIO3_PDDR |= SYNC_MASK;
	while (1) {
		GPIO3_PSOR |= SYNC_MASK;   // HIGH
		for (volatile int i=0;i<10000;i++) {
			__asm__ volatile("nop");
		}
		GPIO3_PCOR &= ~SYNC_MASK;  // LOW
		for (volatile int i=0;i<10000;i++) {
			__asm__ volatile("nop");
		}
	}
#endif
const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio3));

	gpio_pin_configure(gpio_dev, 14, GPIO_OUTPUT);
	GPIO3_PDDR |= SYNC_MASK;
//		GPIO3_PDDR |= 0x0;

	
	gpio_pin_set(gpio_dev, 14, 1);
	k_msleep(100);
	
//	GPIO3_PSOR = SYNC_MASK;
	
while (1) {
//	gpio_pin_toggle(gpio_dev, 14);
	gpio_pin_set(gpio_dev, 14, 1);
	k_msleep(1);
	gpio_pin_set(gpio_dev, 14, 0);
	k_msleep(1);

	GPIO3_PSOR |= SYNC_MASK;   // HIGH
	k_msleep(2);
	GPIO3_PCOR &= ~SYNC_MASK;  // LOW
	k_msleep(2);

}	
	
	/* 何らかのクロック初期化が別途必要（省略） */
	dma_init();
	ctimer_init();

	next_line_time = CTIMER1_TC + LINE_US;
	line = 0;

	while (1) {
		start_line(); /* ライン駆動（CPUはライン粒度のみ関与） */
	}
}
#endif
