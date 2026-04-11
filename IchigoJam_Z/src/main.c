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
