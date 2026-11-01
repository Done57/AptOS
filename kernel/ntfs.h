#ifndef APTOS_NTFS_H
#define APTOS_NTFS_H

#include "types.h"
#include "fs.h"

typedef struct {
    int  dev;
    int  mounted;
    u32  bytes_per_sector;
    u32  sectors_per_cluster;
    u32  cluster_size;
    u64  total_sectors;
    u64  mft_lcn;          /* $MFT 起始簇 */
    u64  mftmirr_lcn;
    u32  mft_record_size;   /* 字节 */
    u32  index_record_size; /* 字节 */
    char label[17];
} ntfs_state_t;

int  ntfs_bind(ntfs_state_t *st, int dev);
int  ntfs_list(ntfs_state_t *st, const char *path, int index, fs_dirent_t *out);
int  ntfs_resolve(ntfs_state_t *st, const char *path, fs_dirent_t *out);
int  ntfs_read(ntfs_state_t *st, const char *path, void *buf, u32 max);
u32  ntfs_total_kb(ntfs_state_t *st);

int  ntfs_format_empty(int dev, const char *label);

#endif
