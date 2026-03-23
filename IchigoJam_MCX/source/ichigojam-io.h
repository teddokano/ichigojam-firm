// Copyright 2014-2024 the IchigoJam authors. All rights reserved. MIT license.
//
// GPIO implementation for FRDM-MCXA153 (MCX-A153)
//
// Pin assignment (Arduino UNO compatible header):
//   OUT1 = P1_4  (D2)   GPIO1 pin4
//   OUT2 = P1_5  (D3)   GPIO1 pin5
//   OUT3 = P1_6  (D4)   GPIO1 pin6
//   OUT4 = P2_0  (D5)   GPIO2 pin0
//   OUT5 = P2_1  (D6)   GPIO2 pin1
//   OUT6 = P2_2  (D7)   GPIO2 pin2
//   IN1  = P0_10 (D8)   GPIO0 pin10   pull-up input
//   IN2  = P0_11 (D9)   GPIO0 pin11   pull-up input
//   IN3  = P0_12 (D10)  GPIO0 pin12   pull-up input
//   IN4  = P0_13 (D11)  GPIO0 pin13   pull-up input
//   BTN  = SW2   P3_29  GPIO3 pin29   pull-up input (active-low)
//   LED  = LED_RED P3_12 (active-low, already managed by board.h)
//
// Notes:
//   - P0_2 / P0_3 are LPUART0 RX/TX — do not use.
//   - P3_12 is the red LED (active-low).
//   - P1_7 is SW3 — avoided for OUT pins.
//   - IN/OUT pins use the GPIO mux (kPORT_MuxAsGpio).
//   - ANA() always returns 0 (no ADC wiring yet).
//   - I2C always returns error (not wired yet).
//   - PWM always returns stub (not wired yet).

#include "board.h"
#include "peripherals.h"
#include "fsl_port.h"
#include "fsl_gpio.h"
#include "fsl_clock.h"
#include "fsl_reset.h"

// ---------- forward declarations ----------
/*S_INLINE*/ void IJB_pwm(int port, int plen, int len);
S_INLINE void pwm_off(int port);
void io_init();
S_INLINE void io_set(int n);
S_INLINE int analog_get(int ch);
S_INLINE int io_get();
S_INLINE void IJB_led(int st);
/*S_INLINE*/ int IJB_in();
S_INLINE void IJB_clo();
S_INLINE int IJB_ana(int n);
/*S_INLINE*/ int IJB_btn(int n);
/*S_INLINE*/ void IJB_out(int port, int st);
S_INLINE int IJB_i2c(uint8 writemode, uint16* param);

int i2c0_init() { return 0; }

// ---------- LED ----------
#define BOARD_LED_GPIO     BOARD_LED_RED_GPIO
#define BOARD_LED_GPIO_PIN BOARD_LED_RED_GPIO_PIN

// ---------- pin tables ----------
// OUT1-6
typedef struct { GPIO_Type *gpio; PORT_Type *port; uint8_t pin; } io_pin_t;

static const io_pin_t OUT_PINS[6] = {
	{ GPIO2, PORT2,  4U },   // OUT1 = P2_4  (D2)
	{ GPIO3, PORT3,  0U },   // OUT2 = P3_0  (D3)
	{ GPIO2, PORT2,  5U },   // OUT3 = P2_5  (D4)
	{ GPIO3, PORT3, 12U },   // OUT4 = P3_12 (D5)
	{ GPIO3, PORT3, 13U },   // OUT5 = P3_13 (D6)
	{ GPIO3, PORT3,  1U },   // OUT6 = P3_1  (D7)
};

// IN1-4
static const io_pin_t IN_PINS[4] = {
	{ GPIO0, PORT0, 10U },  // IN1 = P0_10 (D8)
	{ GPIO0, PORT0, 11U },  // IN2 = P0_11 (D9)
	{ GPIO0, PORT0, 12U },  // IN3 = P0_12 (D10)
	{ GPIO0, PORT0, 13U },  // IN4 = P0_13 (D11)
};

// BTN = SW2 (P3_29, active-low)
#define BTN_GPIO     GPIO3
#define BTN_PORT     PORT3
#define BTN_PIN      29U

// ---------- port_pin_config helpers ----------
static const port_pin_config_t CFG_OUT = {
	kPORT_PullDisable,
	kPORT_LowPullResistor,
	kPORT_FastSlewRate,
	kPORT_PassiveFilterDisable,
	kPORT_OpenDrainDisable,
	kPORT_LowDriveStrength,
	kPORT_NormalDriveStrength,
	kPORT_MuxAsGpio,
	kPORT_InputBufferDisable,
	kPORT_InputNormal,
	kPORT_UnlockRegister,
};

static const port_pin_config_t CFG_IN = {
	kPORT_PullUp,
	kPORT_LowPullResistor,
	kPORT_FastSlewRate,
	kPORT_PassiveFilterDisable,
	kPORT_OpenDrainDisable,
	kPORT_LowDriveStrength,
	kPORT_NormalDriveStrength,
	kPORT_MuxAsGpio,
	kPORT_InputBufferEnable,
	kPORT_InputNormal,
	kPORT_UnlockRegister,
};

// ---------- io_init ----------
void io_init() {
	// Enable clocks for all ports/GPIOs used
	CLOCK_EnableClock(kCLOCK_GatePORT0);
	CLOCK_EnableClock(kCLOCK_GatePORT1);
	CLOCK_EnableClock(kCLOCK_GatePORT2);
	CLOCK_EnableClock(kCLOCK_GatePORT3);
	CLOCK_EnableClock(kCLOCK_GateGPIO0);
	CLOCK_EnableClock(kCLOCK_GateGPIO1);
	CLOCK_EnableClock(kCLOCK_GateGPIO2);
	CLOCK_EnableClock(kCLOCK_GateGPIO3);

	// Release resets — PORT first, then GPIO
	RESET_ReleasePeripheralReset(kPORT0_RST_SHIFT_RSTn);
	RESET_ReleasePeripheralReset(kPORT1_RST_SHIFT_RSTn);
	RESET_ReleasePeripheralReset(kPORT2_RST_SHIFT_RSTn);
	RESET_ReleasePeripheralReset(kPORT3_RST_SHIFT_RSTn);
	RESET_ReleasePeripheralReset(kGPIO0_RST_SHIFT_RSTn);
	RESET_ReleasePeripheralReset(kGPIO1_RST_SHIFT_RSTn);
	RESET_ReleasePeripheralReset(kGPIO2_RST_SHIFT_RSTn);
	RESET_ReleasePeripheralReset(kGPIO3_RST_SHIFT_RSTn);

	// Configure OUT pins as GPIO output, initial LOW
	gpio_pin_config_t out_cfg = { kGPIO_DigitalOutput, 0U };
	for (int i = 0; i < 6; i++) {
		PORT_SetPinConfig(OUT_PINS[i].port, OUT_PINS[i].pin, &CFG_OUT);
		GPIO_PinInit(OUT_PINS[i].gpio, OUT_PINS[i].pin, &out_cfg);
	}

	// Configure IN pins as GPIO input with pull-up
	gpio_pin_config_t in_cfg = { kGPIO_DigitalInput, 0U };
	for (int i = 0; i < 4; i++) {
		PORT_SetPinConfig(IN_PINS[i].port, IN_PINS[i].pin, &CFG_IN);
		GPIO_PinInit(IN_PINS[i].gpio, IN_PINS[i].pin, &in_cfg);
	}

	// Configure BTN (SW2) as input with pull-up
	PORT_SetPinConfig(BTN_PORT, BTN_PIN, &CFG_IN);
	GPIO_PinInit(BTN_GPIO, BTN_PIN, &in_cfg);

	// LED (already initialized by board.h macro, but call for safety)
	LED_RED_INIT(LOGIC_LED_OFF);
}

// ---------- ADC (stub) ----------
#define ANA_THRESHOLD (1024 / 4)
S_INLINE int analog_get(int ch) { return 0; }

// ---------- IN() — read IN1-4 as bits 0-3 ----------
// IchigoJam IN pins are active-high; the physical pull-up means
// unconnected = 1 (high). We return the raw GPIO value (1=high, 0=low).
S_INLINE int io_get() {
	int res = 0;
	for (int i = 0; i < 4; i++) {
		if (GPIO_PinRead(IN_PINS[i].gpio, IN_PINS[i].pin)) {
			res |= (1 << i);
		}
	}
	return res;
}

// ---------- OUT() — set OUT1-6 ----------
// port: 1-6 → index 0-5; port 0 = all pins from bitmask
/*S_INLINE*/ void IJB_out(int port, int st) {
	if (port == 0) {
		// bulk set: bits 0-5 correspond to OUT1-6
		for (int i = 0; i < 6; i++) {
			GPIO_PinWrite(OUT_PINS[i].gpio, OUT_PINS[i].pin,
						  (st >> i) & 1);
		}
	} else if (port >= 1 && port <= 6) {
		GPIO_PinWrite(OUT_PINS[port - 1].gpio, OUT_PINS[port - 1].pin,
					  st ? 1U : 0U);
	}
}

// io_set: IchigoJam internal bulk-write (mirrors IJB_out port=0)
S_INLINE void io_set(int n) {
	IJB_out(0, n);
}

// ---------- IN() BASIC command ----------
/*S_INLINE*/ int IJB_in() {
	return io_get();
}

// ---------- BTN() ----------
// BTN(0) → SW2 state (1 = pressed, active-low hardware)
// BTN(n≠0) → 0 (keyboard BTN not supported without USB HID)
/*S_INLINE*/ int IJB_btn(int n) {
	if (n == 0) {
		return GPIO_PinRead(BTN_GPIO, BTN_PIN) == 0 ? 1 : 0;
	}
	return 0;
}

// ---------- LED() ----------
S_INLINE void IJB_led(int st) {
	if (st) {
		GPIO_PortClear(BOARD_LED_GPIO, 1u << BOARD_LED_GPIO_PIN); // active-low ON
	} else {
		GPIO_PortSet(BOARD_LED_GPIO, 1u << BOARD_LED_GPIO_PIN);   // OFF
	}
}

// ---------- ANA() — stub ----------
S_INLINE int IJB_ana(int n) { return 0; }

// ---------- CLO — reset I/O ----------
S_INLINE void IJB_clo() { io_init(); }

// ---------- PWM — stub ----------
/*S_INLINE*/ void IJB_pwm(int port, int plen, int len) {}
S_INLINE void pwm_off(int port) {}

// ---------- I2C — stub ----------
S_INLINE int IJB_i2c(uint8 writemode, uint16* param) { return 1; }
