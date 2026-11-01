#ifndef APTOS_EXT4_H
#define APTOS_EXT4_H

#include "types.h"
#include "fs.h"

typedef struct {
    int  dev;
    int  mounted;
    u32  block_size;
    u32  blocks_per_group;
    u32  inodes_per_group;
    u32  inode_size;
    u32  block_group_desc_start;   /* 块号 */
    u32  block_count;
    u32  free_blocks;
    char label[17];
    int  has_extents;              /* 支持 extent 树（ext4 特征） */
} ext4_state_t;

int  ext4_bind(ext4_state_t *st, int dev);
int  ext4_list(ext4_state_t *st, const char *path, int index, fs_dirent_t *out);
int  ext4_resolve(ext4_state_t *st, const char *path, fs_dirent_t *out);
int  ext4_read(ext4_state_t *st, const char *path, void *buf, u32 max);
u32  ext4_total_kb(ext4_state_t *st);
u32  ext4_free_kb(ext4_state_t *st);

int  ext4_format_empty(int dev, const char *label);

#endif
