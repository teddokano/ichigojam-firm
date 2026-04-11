// IchigoJam_Z - flash storage HAL using Zephyr NVS
// M0/M1: stub (returns error). Real NVS implemented in M2.

#ifndef __FLASHSTORAGE_H__
#define __FLASHSTORAGE_H__

void flash_init(void) {
}

INLINE int IJB_file(void) {
    return _g.lastfile;
}

int IJB_save(int n, uint8* list, int size) {
    return -1; // not yet implemented (M2)
}

int IJB_load(int n, uint8* list, int sizelimit, int init) {
    if (init) {
        _g.lastfile = n;
    }
    for (int i = 0; i < sizelimit; i++) {
        list[i] = 0;
    }
    return 0; // empty (not yet implemented)
}

#endif // __FLASHSTORAGE_H__
