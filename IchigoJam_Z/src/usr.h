// IchigoJam_Z - USR() machine code call (Zephyr/RP2040)
//
// Same convention as IchigoJam_P:
//   R0 = n          (user argument)
//   R1 = mem        = RAM_AREA - OFFSET_RAMROM  (BASIC→real addr base)
//   R2 = rom        = CHAR_PATTERN              (font ROM base)
//   R3 = fdiv       = __aeabi_uidivmod          (soft-divide helper)
//
// The address `ad` is a BASIC virtual address (#C00 .. #11FF for LIST area).
// We convert it to a real ARM address and set bit0 for Thumb mode.

#ifndef __USR_H__
#define __USR_H__

extern uint64_t __aeabi_uidivmod(unsigned numerator, unsigned denominator);

INLINE int IJB_usr(int ad, int n) {
    if (ad < OFFSET_RAMROM || ad >= OFFSET_RAMROM + SIZE_RAM) {
        return 0;  // address out of RAM range
    }
    // real address = RAM_AREA + (ad - OFFSET_RAMROM), +1 for Thumb mode
    int (*f)(int, void *, void *, void *) =
        (void *)(RAM_AREA + ad - (OFFSET_RAMROM - 1));
    void *mem  = (void *)((uintptr_t)RAM_AREA - OFFSET_RAMROM);  // R1: BASIC→real base
    void *rom  = (void *)CHAR_PATTERN;                // R2: font ROM
    void *fdiv = (void *)__aeabi_uidivmod;            // R3: soft-divide
    return f(n, mem, rom, fdiv);
}

#endif // __USR_H__
