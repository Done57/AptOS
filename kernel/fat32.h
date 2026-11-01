#ifndef APTOS_FAT32_H
#define APTOS_FAT32_H

#include "types.h"
#include "fs.h"

typedef struct {
    int  dev;
    int  mounted;
    u32  bytes_per_sector;
    u32  sectors_per_cluster;
    u32  reserved_sectors;
    u32  num_fats;
    u32  fat_sectors;       /* FAT32: sectors per FAT */
    u32  root_cluster;
    u32  total_sectors;
    u32  data_start;        /* LBA of cluster 2 */
    u32  clusters;
    char label[12];
    u32  fat_start;         /* LBA of first FAT */
} fat32_state_t;

int  fat32_bind(fat32_state_t *st, int dev);
int  fat32_list(fat32_state_t *st, const char *path, int index, fs_dirent_t *out);
int  fat32_resolve(fat32_state_t *st, const char *path, fs_dirent_t *out);
int  fat32_read(fat32_state_t *st, const char *path, void *buf, u32 max);
u32  fat32_total_kb(fat32_state_t *st);
u32  fat32_free_kb(fat32_state_t *st);
int  fat32_format(int dev, const char *label, u32 sectors_per_cluster);

#endif
