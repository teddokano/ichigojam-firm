// IchigoJam_Z - Zephyr/RP2040 port configuration

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Include <stddef.h> first to prevent ichigojam-stddef.h's __STDDEF_H__ guard
// from shadowing GCC's built-in stddef.h (which checks for __STDDEF_H__).
#include <stddef.h>

#define NO_MEMCPY
#include <string.h>

#include "ichigojam-stddef.h"

#define PSG_TICK_FREQ 60
#define PSG_TICK_PER_SEC 60

#define IJB_VER_STR 1.4
#define IJB_TITLE "IchigoJam BASIC " STRING2(IJB_VER_STR) " rp2040 Zephyr\n"

#define VER_PLATFORM PLATFORM_RP2040_ZEPHYR

#define NO_KBD_COMMAND

#define N_FLASH_STORAGE 100

#define EXT_IOT

#endif // __CONFIG_H__
