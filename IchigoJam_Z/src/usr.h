// IchigoJam_Z - USR() machine code call (same as P port)

#ifndef __USR_H__
#define __USR_H__

INLINE int IJB_usr(int ad, int n) {
    typedef int (*fp_t)(int);
    fp_t fp = (fp_t)(ad | 1); // Thumb mode
    return fp(n);
}

#endif // __USR_H__
