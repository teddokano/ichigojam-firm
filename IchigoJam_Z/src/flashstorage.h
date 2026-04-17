// IchigoJam_Z - flash storage HAL using Zephyr NVS

#ifndef __FLASHSTORAGE_H__
#define __FLASHSTORAGE_H__

#include <zephyr/kernel.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>

// NVS key: file n uses ID (n + 1) since NVS ID 0 is reserved
#define NVS_ID(n) ((uint16_t)((n) + 1))

static struct nvs_fs _nvs;
static bool _nvs_ready = false;

void flash_init(void)
{
    const struct flash_area *fa;
    struct flash_pages_info info;

    if (flash_area_open(PARTITION_ID(storage_partition), &fa) != 0) {
        return;
    }

    _nvs.flash_device = flash_area_get_device(fa);
    _nvs.offset       = fa->fa_off;

    if (flash_get_page_info_by_offs(_nvs.flash_device, _nvs.offset, &info) != 0) {
        flash_area_close(fa);
        return;
    }

    _nvs.sector_size  = info.size;
    _nvs.sector_count = (uint16_t)(fa->fa_size / info.size);
    flash_area_close(fa);

    if (nvs_mount(&_nvs) == 0) {
        _nvs_ready = true;
    }
}

INLINE int IJB_file(void) {
    return _g.lastfile;
}

// Returns 0 on success, -1 on error
int IJB_save(int n, uint8 *list, int size)
{
    _g.lastfile = n;

    if (!_nvs_ready) {
        return -1;
    }
    if (n < 0 || n >= N_FLASH_STORAGE) {
        return -1;
    }

    ssize_t ret = nvs_write(&_nvs, NVS_ID(n), list, (size_t)size);
    return (ret >= 0) ? 0 : -1;
}

// Returns bytes read on success, -1 if empty / error
int IJB_load(int n, uint8 *list, int sizelimit, int init)
{
    if (init) {
        _g.lastfile = n;
    }

    if (!_nvs_ready || n < 0 || n >= N_FLASH_STORAGE) {
        // Return empty program
        *(uint16 *)list = 0;
        return -1;
    }

    ssize_t ret = nvs_read(&_nvs, NVS_ID(n), list, (size_t)sizelimit);
    if (ret < 0) {
        // Entry not found: return empty
        *(uint16 *)list = 0;
        return -1;
    }
    return (int)ret;
}

#endif // __FLASHSTORAGE_H__
