/*
 * ext2/3/4 只读驱动（同一套 on-disk 基础结构）。
 *
 * 超级块在偏移 1024，魔数 0xEF53。块组描述符表跟在后面。
 * 文件数据定位支持两种方式：
 *   - 直接/间接块（ext2/3 及 ext4 小文件）
 *   - extent 树（ext4，inode 里 60 字节根节点，叶节点指向数据）
 * 目录是线性 dirent 列表（本驱动不实现 hash 树索引的加速查找，
 * 线性扫描对测试盘足够，且 hash 树目录在 htree 关闭时也是线性的）。
 */
#include "ext4.h"
#include "blkdev.h"
#include "klog.h"
#include "util.h"

#define EXT2_MAGIC 0xEF53
#define S_IFMT     0xF000
#define S_IFDIR    0x4000
#define S_IFREG    0x8000

/* ---- 超级块（偏移 1024，1024 字节结构，只用前 120 字节） ---- */
typedef struct PACKED {
    u32 inodes_count;
    u32 blocks_count;
    u32 r_blocks_count;
    u32 free_blocks_count;
    u32 free_inodes_count;
    u32 first_data_block;
    u32 log_block_size;       /* block size = 1024 << this */
    u32 log_frag_size;
    u32 blocks_per_group;
    u32 frags_per_group;
    u32 inodes_per_group;
    u32 mtime;
    u32 wtime;
    u16 mnt_count;
    s16 max_mnt_count;
    u16 magic;
    /* ... 后面还有字段，按需在原始缓冲里取 */
} sb_core_t;

/* ---- 块组描述符（32 字节，ext4 64 位下 64 字节，取前 32） ---- */
typedef struct PACKED {
    u32 block_bitmap_lo;
    u32 inode_bitmap_lo;
    u32 inode_table_lo;
    u16 free_blocks_count_lo;
    u16 free_inodes_count_lo;
    u16 used_dirs_count_lo;
    u16 pad;
    u32 reserved[3];
} bgd_t;

/* ---- inode（只取本驱动用的字段） ---- */
typedef struct PACKED {
    u16 mode;
    u16 uid;
    u32 size_lo;
    u32 atime;
    u32 ctime;
    u32 mtime;
    u32 dtime;
    u16 gid;
    u16 links;
    u32 blocks_lo;      /* 512B sectors */
    u32 flags;
    u32 osd1;
    u32 block[15];      /* 0-11 direct, 12 indirect, 13 dbl, 14 tpl;
                           ext4: 若 flags 有 EXTENTS 位则这里是 extent 根 */
    u32 generation;
    u32 file_acl_lo;
    u32 size_hi;
    u32 extra[10];
} inode_t;

#define INODE_EXTENTS_FL 0x00080000

/* extent 根/节点头 */
typedef struct PACKED {
    u16 magic;      /* 0xF30A */
    u16 entries;
    u16 max;
    u16 depth;
    u32 generation;
} ext_hdr_t;

typedef struct PACKED {
    u32 leaf;       /* 逻辑块 */
    u16 len;
    u16 start_hi;
    u32 start_lo;
} ext_idx_t;        /* depth>0 是索引，depth=0 同布局但 start 是物理块 */

#define RD(st, lba, n, buf)  ext4_read_blocks((st), (lba), (n), (buf))

/* 块号 -> 扇区号换算 + 读 */
static int ext4_read_blocks(ext4_state_t *st, u32 block, u32 nblocks, void *buf)
{
    u32 ssz = 512;
    return blk_read(st->dev, block * (st->block_size / ssz),
                    nblocks * (st->block_size / ssz), buf);
}

static int ext4_read_block(ext4_state_t *st, u32 block, void *buf)
{
    return ext4_read_blocks(st, block, 1, buf);
}

int ext4_bind(ext4_state_t *st, int dev)
{
    const blk_info_t *bi = blk_info(dev);
    u8 sbraw[1024];

    memset(st, 0, sizeof(*st));
    if (!bi || bi->sector_size != 512)
        return -1;
    if (blk_read(dev, 2, 2, sbraw) != 0)   /* 偏移 1024 = 扇区 2 */
        return -1;

    sb_core_t *sb = (sb_core_t *)sbraw;
    if (sb->magic != EXT2_MAGIC)
        return -1;
    if (sb->log_block_size > 3)
        return -1;

    st->dev         = dev;
    st->block_size  = 1024u << sb->log_block_size;
    st->blocks_per_group = sb->blocks_per_group;
    st->inodes_per_group = sb->inodes_per_group;
    st->block_count = sb->blocks_count;
    st->free_blocks = sb->free_blocks_count;

    /* inode 大小：偏移 88 处的 u16（ext2 默认 128） */
    u16 inode_size = (u16)sbraw[88] | ((u16)sbraw[89] << 8);
    st->inode_size = inode_size ? inode_size : 128;

    /* 第一个数据块之后的块就是块组描述符表 */
    st->block_group_desc_start = sb->first_data_block + 1;

    /* 卷名：偏移 120，16 字节 */
    memcpy(st->label, sbraw + 120, 16);
    st->label[16] = 0;

    /* 是否 ext4（ro_compat 特性位有 extents）—— 简化：看 inode 里的
     * EXTENTS 标志逐个判定，bind 时统一按 extent 能力处理 */
    st->has_extents = 1;
    st->mounted = 1;
    return 0;
}

/* 读第 group 组的块组描述符 */
static int read_bgd(ext4_state_t *st, u32 group, bgd_t *out)
{
    u8 buf[1024];
    u32 per_block = st->block_size / 32;
    u32 block = st->block_group_desc_start + group / per_block;
    u32 off   = (group % per_block) * 32;

    if (st->block_size > sizeof(buf))
        return -1;
    if (ext4_read_block(st, block, buf) != 0)
        return -1;
    memcpy(out, buf + off, sizeof(bgd_t));
    return 0;
}

static int read_inode(ext4_state_t *st, u32 ino, inode_t *out)
{
    u32 group    = (ino - 1) / st->inodes_per_group;
    u32 index    = (ino - 1) % st->inodes_per_group;
    bgd_t bg;

    if (read_bgd(st, group, &bg) != 0)
        return -1;

    static u8 table[8192];   /* 一个 inode 表块（块大小上限 8K） */
    if (st->block_size > sizeof(table))
        return -1;
    u32 table_block = bg.inode_table_lo;
    u32 inodes_per_block = st->block_size / st->inode_size;
    u32 blk = table_block + index / inodes_per_block;
    u32 off = (index % inodes_per_block) * st->inode_size;

    if (ext4_read_block(st, blk, table) != 0)
        return -1;

    /* inode_t 比 ext2 的 128 字节 inode 大（多取了 size_hi 等尾部
     * 字段）。直接 memcpy sizeof(inode_t) 会读进相邻 inode，所以
     * 先清零、只拷 min(inode_size, sizeof) 字节。*/
    memset(out, 0, sizeof(*out));
    u32 copy = st->inode_size < sizeof(*out) ? st->inode_size : (u32)sizeof(*out);
    memcpy(out, table + off, copy);
    return 0;
}

/* extent 树：把逻辑块映射到物理块；depth==0 的叶子直接给答案 */
static int extent_lookup(ext4_state_t *st, const inode_t *ino,
                         u32 lblock, u32 *phys)
{
    u8 node[4096];

    /* 根在 inode.block 里，60 字节 = 头 12 + 4 项 x 12 */
    ext_hdr_t *h = (ext_hdr_t *)(void *)ino->block;
    if (h->magic != 0xF30A)
        return -1;

    if (h->depth == 0) {
        /* block 是 u32[15]：直接 +12 会偏移 48 字节。
         * extent 项紧跟 12 字节的头，必须按字节偏移。*/
        ext_idx_t *ix = (ext_idx_t *)((const u8 *)ino->block + 12);
        for (u16 i = 0; i < h->entries; i++) {
            u32 leaf = ix[i].leaf;
            u16 len  = ix[i].len ? ix[i].len : 0x8000;  /* 32768 = 未初始化 */
            if (lblock >= leaf && lblock < leaf + len) {
                *phys = (ix[i].start_lo | ((u32)ix[i].start_hi << 16))
                      + (lblock - leaf);
                return 0;
            }
        }
        return -1;   /* 稀疏洞 */
    }

    /* 内部节点：读入再查 */
    if (st->block_size > sizeof(node))
        return -1;
    ext_idx_t *ix = (ext_idx_t *)((const u8 *)ino->block + 12);
    for (u16 i = 0; i < h->entries; i++) {
        u32 leaf = ix[i].leaf;
        u16 len  = ix[i].len;
        if (lblock >= leaf && (len == 0 || lblock < leaf + len)) {
            u32 node_block = ix[i].start_lo | ((u32)ix[i].start_hi << 16);
            if (ext4_read_block(st, node_block, node) != 0)
                return -1;
            ext_hdr_t *nh = (ext_hdr_t *)node;
            if (nh->magic != 0xF30A)
                return -1;
            ext_idx_t *nx = (ext_idx_t *)(node + 12);
            for (u16 k = 0; k < nh->entries; k++) {
                u32 lf = nx[k].leaf;
                u16 l2 = nx[k].len ? nx[k].len : 1;
                if (lblock >= lf && lblock < lf + l2) {
                    *phys = (nx[k].start_lo | ((u32)nx[k].start_hi << 16))
                          + (lblock - lf);
                    return 0;
                }
            }
            return -1;
        }
    }
    return -1;
}

/* 传统块映射：0-11 直接，12 一级，13 二级，14 三级 */
static int trad_lookup(ext4_state_t *st, const inode_t *ino,
                       u32 lblock, u32 *phys)
{
    u32 ptrs = st->block_size / 4;
    u8 blkbuf[4096];

    if (lblock < 12) {
        u32 p = ino->block[lblock];
        if (!p) return -1;
        *phys = p;
        return 0;
    }

    u32 rel = lblock - 12;
    if (rel < ptrs) {                       /* 一级 */
        u32 ib = ino->block[12];
        if (!ib) return -1;
        if (ext4_read_block(st, ib, blkbuf) != 0) return -1;
        u32 p = ((u32 *)blkbuf)[rel];
        if (!p) return -1;
        *phys = p;
        return 0;
    }
    rel -= ptrs;
    if (rel < ptrs * ptrs) {                /* 二级 */
        u32 ib = ino->block[13];
        if (!ib) return -1;
        if (ext4_read_block(st, ib, blkbuf) != 0) return -1;
        u32 l1 = ((u32 *)blkbuf)[rel / ptrs];
        if (!l1) return -1;
        if (ext4_read_block(st, l1, blkbuf) != 0) return -1;
        u32 p = ((u32 *)blkbuf)[rel % ptrs];
        if (!p) return -1;
        *phys = p;
        return 0;
    }
    return -1;   /* 三级间接不做：测试盘远用不到 */
}

static int data_block(ext4_state_t *st, const inode_t *ino,
                      u32 lblock, u32 *phys)
{
    if (ino->flags & INODE_EXTENTS_FL)
        return extent_lookup(st, ino, lblock, phys);
    return trad_lookup(st, ino, lblock, phys);
}

/* ---- 目录 dirent ---- */
typedef struct PACKED {
    u32 inode;
    u16 rec_len;
    u8  name_len;
    u8  file_type;
    /* name follows */
} dirent_t;

/* 按路径解析，返回 inode 号（0 失败）；out 可选带 dirent 信息 */
static u32 resolve_ino(ext4_state_t *st, const char *path,
                       inode_t *out_ino, u32 *out_size, u8 *out_type)
{
    char comp[256];
    const char *p = path;
    u32 cur = 2;    /* 根 inode 固定 2 */
    inode_t ino;

    if (read_inode(st, cur, &ino) != 0)
        return 0;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1)
            comp[n++] = *p++;
        comp[n] = 0;

        if (strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            /* 目录头两个 dirent 固定是 . 和 .. */
            u8 dbuf[4096];
            if (st->block_size > sizeof(dbuf)) return 0;
            u32 pb;
            if (data_block(st, &ino, 0, &pb) != 0) return 0;
            if (ext4_read_block(st, pb, dbuf) != 0) return 0;
            dirent_t *d1 = (dirent_t *)dbuf;
            dirent_t *d2 = (dirent_t *)(dbuf + d1->rec_len);
            cur = d2->inode;
            if (read_inode(st, cur, &ino) != 0) return 0;
            continue;
        }

        /* 线性扫目录找 comp */
        u8 dbuf[4096];
        if (st->block_size > sizeof(dbuf)) return 0;
        int found = 0;
        u32 f_ino = 0;
        u64 dir_size = ((u64)ino.size_hi << 32) | ino.size_lo;

        for (u32 lb = 0; lb * (u64)st->block_size < dir_size; lb++) {
            u32 pb;
            if (data_block(st, &ino, lb, &pb) != 0)
                break;
            if (ext4_read_block(st, pb, dbuf) != 0)
                break;
            u32 off = 0;
            while (off + 8 <= st->block_size) {
                dirent_t *d = (dirent_t *)(dbuf + off);
                if (d->rec_len < 8 || off + d->rec_len > st->block_size)
                    break;
                if (d->inode != 0) {
                    int nl = d->name_len;
                    if (nl == n && memcmp(dbuf + off + 8, comp, (size_t)nl) == 0) {
                        f_ino = d->inode;
                        found = 1;
                        break;
                    }
                }
                off += d->rec_len;
            }
            if (found) break;
        }
        if (!found)
            return 0;

        if (read_inode(st, f_ino, &ino) != 0)
            return 0;
        cur = f_ino;
    }

    if (out_ino)  *out_ino = ino;
    if (out_size) *out_size = (u32)(((u64)ino.size_hi << 32) | ino.size_lo);
    if (out_type) *out_type = ((ino.mode & S_IFMT) == S_IFDIR) ? FS_FLAG_DIR : 0;
    return cur;
}

int ext4_resolve(ext4_state_t *st, const char *path, fs_dirent_t *out)
{
    inode_t ino;
    u32 size;
    u8  type;

    if (path[0] == 0 || strcmp(path, "/") == 0) {
        if (out) {
            memset(out, 0, sizeof(*out));
            out->flags = FS_FLAG_DIR;
        }
        return 0;
    }
    if (!resolve_ino(st, path, &ino, &size, &type))
        return -1;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->flags = type;
        out->size = size;
        out->first_block = 0;
    }
    return 0;
}

int ext4_list(ext4_state_t *st, const char *path, int index, fs_dirent_t *out)
{
    inode_t ino;
    u32 size;
    u8  type;

    if (path[0] == 0 || strcmp(path, "/") == 0) {
        if (read_inode(st, 2, &ino) != 0)
            return -1;
    } else {
        if (!resolve_ino(st, path, &ino, &size, &type))
            return -1;
        if (((ino.mode & S_IFMT) != S_IFDIR))
            return -1;
    }

    u64 dir_size = ((u64)ino.size_hi << 32) | ino.size_lo;
    u8 dbuf[4096];
    if (st->block_size > sizeof(dbuf))
        return -1;

    int seen = 0;
    for (u32 lb = 0; lb * (u64)st->block_size < dir_size; lb++) {
        u32 pb = 0;
        int dbr = data_block(st, &ino, lb, &pb);
        if (dbr != 0)
            break;
        if (ext4_read_block(st, pb, dbuf) != 0)
            break;
        u32 off = 0;
        while (off + 8 <= st->block_size) {
            dirent_t *d = (dirent_t *)(dbuf + off);
            if (d->rec_len < 8 || off + d->rec_len > st->block_size)
                break;
            if (d->inode != 0) {
                int nl = d->name_len;
                if (seen == index) {
                    memset(out, 0, sizeof(*out));
                    if (nl >= (int)sizeof(out->name))
                        nl = sizeof(out->name) - 1;
                    memcpy(out->name, dbuf + off + 8, (size_t)nl);
                    out->name[nl] = 0;
                    out->first_block = d->inode;
                    inode_t sub;
                    if (read_inode(st, d->inode, &sub) == 0) {
                        out->size = (u32)(((u64)sub.size_hi << 32) | sub.size_lo);
                        out->flags = ((sub.mode & S_IFMT) == S_IFDIR)
                                   ? FS_FLAG_DIR : 0;
                    }
                    return 0;
                }
                seen++;
            }
            off += d->rec_len;
        }
    }
    return -1;
}

int ext4_read(ext4_state_t *st, const char *path, void *buf, u32 max)
{
    inode_t ino;
    u32 size;
    u8  type;

    if (!resolve_ino(st, path, &ino, &size, &type))
        return -1;
    if ((ino.mode & S_IFMT) == S_IFDIR)
        return -1;

    u32 left = size < max ? size : max;
    u32 got = 0;
    u8 *dst = buf;
    u32 lblock = 0;

    while (got < left) {
        u32 chunk = st->block_size;
        if (chunk > left - got)
            chunk = left - got;
        u32 pb;
        if (data_block(st, &ino, lblock, &pb) != 0)
            break;   /* 稀疏洞或链尾：按 0 填充 */
        memset(dst, 0, chunk);
        u8 blkbuf[4096];
        if (st->block_size > sizeof(blkbuf))
            return -1;
        if (ext4_read_block(st, pb, blkbuf) != 0)
            return -1;
        memcpy(dst, blkbuf, chunk);
        dst += chunk;
        got += chunk;
        lblock++;
    }
    return (int)got;
}

u32 ext4_total_kb(ext4_state_t *st)
{
    return st->block_count / (2048 / st->block_size);
}

u32 ext4_free_kb(ext4_state_t *st)
{
    return st->free_blocks / (2048 / st->block_size);
}


/* ---- ext2/4 空卷骨架（1024B 块，单组）----
 * 超级块（魔数 0xEF53 @1024+0x38）+ 组描述符 + 位图 + 只含根目录
 * 的 inode 表（inode2 = 目录，extent 指向一个空数据块）。
 * 只读驱动可挂载成空卷。*/
int ext4_format_empty(int dev, const char *label)
{
    const blk_info_t *bi = blk_info(dev);
    u8 sb[1024];

    if (!bi || bi->sector_size != 512 || !bi->writable)
        return -1;
    if (bi->sectors < 16384)         /* 至少 8MB */
        return -2;

    const u32 bs = 1024;
    const u32 total_blocks = bi->sectors / 2;
    const u32 bmp_block = 3, ibmp_block = 4, itable = 5;
    const u32 ipg = 2048, isz = 128;
    const u32 itable_blocks = (ipg * isz + bs - 1) / bs;

    /* ---- 超级块（块 1）---- */
    memset(sb, 0, sizeof(sb));
    fmt_le32(sb + 0x00, 16);         /* inodes_count 占位 */
    fmt_le32(sb + 0x04, total_blocks);
    fmt_le32(sb + 0x0C, 0);          /* free 略（探测不查） */
    fmt_le32(sb + 0x14, 1024);       /* first_data_block */
    fmt_le32(sb + 0x18, 0);          /* log_bs = 1024 */
    fmt_le32(sb + 0x1C, 0);          /* log_frag */
    fmt_le32(sb + 0x20, total_blocks);
    fmt_le32(sb + 0x24, total_blocks);
    fmt_le32(sb + 0x28, ipg);
    fmt_le32(sb + 0x2C, 0);          /* frags_per_group */
    fmt_le32(sb + 0x30, ipg);
    fmt_le16(sb + 0x38, 0xEF53);
    fmt_le16(sb + 0x3A, 1);          /* state clean */
    fmt_le16(sb + 0x3C, 0);          /* errors */
    fmt_le16(sb + 0x44, 0);          /* rev */
    /* rev1 字段 @0x4C.. */
    fmt_le32(sb + 0x4C, 1);          /* rev_level = 动态 */
    fmt_le16(sb + 0x54, 0);          /* first_ino */
    fmt_le16(sb + 0x58, isz);
    /* 卷标 @0x78 16 字节 */
    for (int i = 0; i < 16 && label[i]; i++)
        sb[0x78 + i] = (u8)label[i];
    if (blk_write(dev, bs / 512, 2, sb) != 0)    /* 块1 = 扇区2..3 */
        return -3;

    /* ---- 组描述符（块 2）---- */
    static u8 gd[1024];
    memset(gd, 0, sizeof(gd));
    fmt_le32(gd + 0x00, 0);          /* block_bitmap = 3 */
    fmt_le32(gd + 0x04, 0);
    fmt_le32(gd + 0x08, 0);
    fmt_le32(gd + 0x0C, 0);
    fmt_le16(gd + 0x00, (u16)(bmp_block));      /* 偏移 0: block bitmap */
    fmt_le16(gd + 0x02, (u16)(ibmp_block));
    fmt_le16(gd + 0x04, (u16)(itable));
    fmt_le16(gd + 0x06, (u16)(ipg - 12));       /* free inodes 略 */
    fmt_le16(gd + 0x08, 0);
    fmt_le16(gd + 0x0A, 1);                     /* used_dirs = 1 */
    if (blk_write(dev, 2 * bs / 512, 2, gd) != 0)
        return -3;

    /* ---- 位图（块 3、4）：前 10 块/前 12 inode 已用 ---- */
    static u8 bmp[1024];
    memset(bmp, 0, sizeof(bmp));
    for (u32 i = 0; i <= itable + itable_blocks; i++)
        bmp[i / 8] |= (u8)(1 << (i % 8));
    if (blk_write(dev, bmp_block * bs / 512, 2, bmp) != 0)
        return -3;
    memset(bmp, 0, sizeof(bmp));
    for (u32 i = 0; i < 12; i++)
        bmp[i / 8] |= (u8)(1 << (i % 8));
    if (blk_write(dev, ibmp_block * bs / 512, 2, bmp) != 0)
        return -3;

    /* ---- inode 表（块 5..）：inode2 = 根目录（extent→块 8）---- */
    static u8 itab[4096];   /* 只填 inode 1..2，够空卷 */
    memset(itab, 0, sizeof(itab));
    u8 *r = itab + 1 * isz;          /* inode2 = index 1 */
    fmt_le16(r + 0x00, 0x41ED);      /* mode: dir 755 */
    fmt_le16(r + 0x1A, 2);           /* nlink */
    fmt_le32(r + 0x04, bs);          /* size */
    fmt_le32(r + 0x1C, 1);           /* links 略 */
    fmt_le32(r + 0x20, 0x80000);     /* flags = extents */
    /* extent 根：头(12B) + 1 项(12B) */
    fmt_le16(r + 0x28, 0xF30A);
    fmt_le16(r + 0x2A, 1);           /* entries */
    fmt_le16(r + 0x2C, 4);           /* max */
    fmt_le16(r + 0x2E, 0);           /* depth */
    fmt_le32(r + 0x30, 0);           /* gen */
    fmt_le32(r + 0x34, 0);           /* leaf=逻辑块0 */
    fmt_le16(r + 0x38, 1);           /* len=1 */
    fmt_le16(r + 0x3A, 0);           /* start_hi */
    fmt_le32(r + 0x3C, 8);           /* start_lo = 块8（元数据之后） */
    if (blk_write(dev, itable * bs / 512, 4, itab) != 0)
        return -3;

    /* ---- 根目录数据块（块 8）：. 和 .. 两项 ---- */
    static u8 db[1024];
    memset(db, 0, sizeof(db));
    u32 o = 0;
    for (u32 ino = 2; ino <= 3; ino++) {
        u16 rl = 12;
        fmt_le32(db + o, ino);
        fmt_le16(db + o + 4, rl);
        db[o + 6] = 1;
        db[o + 7] = 2;               /* ftype dir */
        db[o + 8] = ino == 2 ? '.' : '.';
        db[o + 9] = ino == 3 ? '.' : 0;
        o += rl;
    }
    if (blk_write(dev, 8 * bs / 512, 2, db) != 0)
        return -3;
    return 0;
}
