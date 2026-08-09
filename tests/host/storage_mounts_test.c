#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "flash_storage.h"
#include "solar_os_board_storage.h"
#include "solar_os_ramfs.h"
#include "solar_os_storage.h"

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0) {
        const size_t copy = len >= size ? size - 1 : len;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

size_t solar_os_board_storage_block_count(void)
{
    return 2;
}

bool solar_os_board_storage_get_block(size_t index, solar_os_board_storage_block_t *block)
{
    if (block == NULL || index >= solar_os_board_storage_block_count()) {
        return false;
    }

    memset(block, 0, sizeof(*block));
    if (index == 0) {
        strlcpy(block->name, "sd0", sizeof(block->name));
        block->type = SOLAR_OS_BOARD_STORAGE_BLOCK_DISK;
        return true;
    }

    strlcpy(block->name, "sd0p1", sizeof(block->name));
    strlcpy(block->mount_point, "/sdcard", sizeof(block->mount_point));
    block->type = SOLAR_OS_BOARD_STORAGE_BLOCK_PARTITION;
    block->mounted = true;
    block->mountable = true;
    block->logical_volume = 0;
    return true;
}

bool solar_os_board_storage_is_mounted(void)
{
    return true;
}

const char *solar_os_board_storage_mount_point(void)
{
    return "/sdcard";
}

bool flash_storage_is_mounted(void)
{
    return true;
}

const char *flash_storage_mount_point(void)
{
    return "/flash";
}

uint8_t flash_storage_logical_volume(void)
{
    return 1;
}

uint64_t flash_storage_size_bytes(void)
{
    return 600U * 1024U;
}

size_t solar_os_ramfs_mount_count(void)
{
    return 1;
}

bool solar_os_ramfs_get_info(size_t index, solar_os_ramfs_info_t *info)
{
    if (index != 0 || info == NULL) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    strlcpy(info->mount_point, "/ram", sizeof(info->mount_point));
    return true;
}

static void assert_mount(size_t index,
                         const char *name,
                         const char *mount_point,
                         solar_os_storage_mount_type_t type)
{
    solar_os_storage_mount_info_t mount;
    assert(solar_os_storage_get_mount(index, &mount));
    assert(strcmp(mount.name, name) == 0);
    assert(strcmp(mount.mount_point, mount_point) == 0);
    assert(mount.type == type);
}

int main(void)
{
    assert(solar_os_storage_block_count() == 3);
    assert(solar_os_storage_mount_count() == 3);
    assert_mount(0, "sd0p1", "/sdcard", SOLAR_OS_STORAGE_MOUNT_SD);
    assert_mount(1, "flash", "/flash", SOLAR_OS_STORAGE_MOUNT_FLASH);
    assert_mount(2, "ramfs", "/ram", SOLAR_OS_STORAGE_MOUNT_RAMFS);

    solar_os_storage_mount_info_t mount;
    assert(!solar_os_storage_get_mount(3, &mount));

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    assert(solar_os_storage_default_path(".player", path, sizeof(path)) == ESP_OK);
    assert(strcmp(path, "/sdcard/.player") == 0);

    puts("storage mount tests: ok");
    return 0;
}
