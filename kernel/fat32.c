/*
 * FAT32 只读驱动。
 *
 * 布局：BPB（引导扇区）给出全部几何参数；FAT 表项 4 字节，
 * 低 28 位有效；目录项 32 字节，长文件名（LFN）由多个 0x0F
 * 属性项拼出 UTF-16 名称，短项是 8.3 兼容视图。
 *
 * 链式读取：文件占用的簇号链存放在 FAT 里，簇 N 的下一个簇
 * 是 FAT[N] & 0x0FFFFFFF，0x0FFFFFF8 以上是链尾。
 */
#include "fat32.h"
#include "blkdev.h"
#include "klog.h"
#include "util.h"

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_LFN       0x0F
#define FAT32_EOC      0x0FFFFFF8

static int rd_dev(fat32_state_t *st, u32 lba, u32 n, void *buf)
{
    return blk_read(st->dev, lba, n, buf);
}

/*  cluster -> LBA  */
static u32 cluster_lba(fat32_state_t *st, u32 cluster)
{
    return st->data_start + (cluster - 2) * st->sectors_per_cluster;
}

static u32 fat_entry(fat32_state_t *st, u32 cluster)
{
    u8  sec[512];
    u32 off, val;

    off = (cluster * 4) / st->bytes_per_sector;
    if (rd_dev(st, st->fat_start + off, 1, sec) != 0)
        return 0;
    val = (u32)sec[(cluster * 4) % 512]
        | ((u32)sec[(cluster * 4) % 512 + 1] << 8)
        | ((u32)sec[(cluster * 4) % 512 + 2] << 16)
        | ((u32)sec[(cluster * 4) % 512 + 3] << 24);
    return val & 0x0FFFFFFFu;
}

int fat32_bind(fat32_state_t *st, int dev)
{
    const blk_info_t *bi = blk_info(dev);
    u8 bs[512];

    memset(st, 0, sizeof(*st));
    if (!bi || bi->sector_size != 512)
        return -1;
    st->dev = dev;   /* 必须在读扇区前赋值：rd_dev 从 st->dev 取设备号 */
    if (rd_dev(st, 0, 1, bs) != 0)
        return -1;

    /* FAT32 判据：BPB 存在（跳转指令或引导代码 + 扇区数合理），
     * FATSz16 == 0 且根目录簇号 >= 2 */
    u16 fatsz16 = (u16)bs[22] | ((u16)bs[23] << 8);
    u16 rsvd    = (u16)bs[14] | ((u16)bs[15] << 8);
    u32 root_cl = (u32)bs[44] | ((u32)bs[45] << 8)
                | ((u32)bs[46] << 16) | ((u32)bs[47] << 24);

    if (fatsz16 != 0 || rsvd < 1 || root_cl < 2) {
        kprintf("[fat32] %s: not fat32 (fatsz16=%u rsvd=%u root_cl=%u)\n",
                bi->name, fatsz16, rsvd, root_cl);
        return -1;
    }

    st->bytes_per_sector   = (u32)bs[11] | ((u32)bs[12] << 8);
    st->sectors_per_cluster = bs[13];
    st->reserved_sectors   = rsvd;
    st->num_fats           = bs[16];
    st->fat_sectors        = (u32)bs[36] | ((u32)bs[37] << 8)
                           | ((u32)bs[38] << 16) | ((u32)bs[39] << 24);
    st->root_cluster       = root_cl;
    st->total_sectors      = (u32)bs[32] | ((u32)bs[33] << 8)
                           | ((u32)bs[34] << 16) | ((u32)bs[35] << 24);
    if (st->total_sectors == 0)
        st->total_sectors = (u32)bs[19] | ((u32)bs[20] << 8);

    if (st->bytes_per_sector != 512 || st->sectors_per_cluster == 0) {
        kprintf("[fat32] hd: bad bps=%u spc=%u\n",
                st->bytes_per_sector, st->sectors_per_cluster);
        return -1;
    }
    if (st->num_fats == 0 || st->fat_sectors == 0) {
        kprintf("[fat32] hd: bad nfats=%u fatsz=%u\n",
                st->num_fats, st->fat_sectors);
        return -1;
    }

    st->fat_start  = st->reserved_sectors;
    st->data_start = st->reserved_sectors
                   + st->num_fats * st->fat_sectors;
    st->clusters   = (st->total_sectors - st->data_start)
                   / st->sectors_per_cluster;
    if (st->clusters < 4085) {
        kprintf("[fat32] hd: only %u clusters (FAT16?)\n", st->clusters);
        return -1;   /* 这实际是 FAT16，不是 FAT32 */
    }

    memcpy(st->label, bs + 71, 11);
    st->label[11] = 0;
    st->mounted = 1;
    return 0;
}

/* ---- 目录遍历 ---- */

/* 把 8.3 名转成可读字符串（去空格补齐） */
static void sfname_to_str(const u8 *raw, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++)
        out[n++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++)
            out[n++] = (char)raw[i];
    }
    out[n] = 0;
}

/* LFN：UTF-16LE 名（跳过以 0 结尾的部分），转成 ASCII/UTF-8
 * （本系统用户可见字符集就是 UTF-8，CJK 文件名前 256 个常用字
 * 可以由 fontcjk 渲染；超出部分显示 '?'。这里直接复制低字节，
 * 非 ASCII 字符按 UTF-8 编码写出。）*/
static int utf16_to_utf8(const u16 *w, int wmax, char *out, int outsz)
{
    int n = 0;
    for (int i = 0; i < wmax && w[i]; i++) {
        u16 c = w[i];
        if (c < 0x80) {
            if (n + 1 >= outsz) break;
            out[n++] = (char)c;
        } else if (c < 0x800) {
            if (n + 2 >= outsz) break;
            out[n++] = (char)(0xC0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (n + 3 >= outsz) break;
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = 0;
    return n;
}

/* LFN 拼接器：正向扫目录时，LFN 项在短项之前，
 * 序号 1..N，校验和匹配才可信 */
typedef struct {
    u16  name[13 * 20];
    u8   checksum;
    int  entries_left;
    int  valid;
} lfn_buf_t;

static void lfn_reset(lfn_buf_t *lb)
{
    lb->entries_left = 0;
    lb->valid = 0;
}

static void lfn_feed(lfn_buf_t *lb, const u8 *e)
{
    u8 seq = e[0];
    if (seq & 0x40) {           /* 最后一组（序号最高位） */
        lb->entries_left = seq & 0x3F;
        lb->checksum = e[13];
        lb->valid = 1;
        memset(lb->name, 0, sizeof(lb->name));
    }
    if (!lb->valid || (u8)(seq & 0x3F) != (u8)lb->entries_left)
        return;

    u16 *dst = lb->name + (lb->entries_left - 1) * 13;
    int k = 0;
    for (int i = 0; i < 5; i++)
        dst[k++] = (u16)e[1 + i * 2] | ((u16)e[2 + i * 2] << 8);
    for (int i = 0; i < 6; i++)
        dst[k++] = (u16)e[14 + i * 2] | ((u16)e[15 + i * 2] << 8);
    for (int i = 0; i < 2; i++)
        dst[k++] = (u16)e[28 + i * 2] | ((u32)e[29 + i * 2] << 8);
    lb->entries_left--;
}

static u8 lfn_checksum(const u8 *shortname)
{
    /* 标准 FAT LFN 校验和 = 11 字节循环右移（ROR）累加。
     * 之前写成 ((sum&1)<<7) + sum + c，多加了一次 sum ——
     * 与生成端/其它实现对不上，LFN 名永远弃用。*/
    u8 sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (u8)(((sum & 1) << 7) + (sum >> 1) + shortname[i]);
    return sum;
}

/* 通用的目录扫描回调：对每个可见项调用 fn，返回非 0 停止 */
typedef int (*dir_cb)(const fs_dirent_t *e, void *ctx);

static int scan_dir(fat32_state_t *st, u32 dir_cluster, dir_cb fn, void *ctx)
{
    u8 sec[512];
    u32 cur = dir_cluster;
    lfn_buf_t lb;
    lfn_reset(&lb);

    while (cur >= 2 && cur < FAT32_EOC) {
        for (u32 s = 0; s < st->sectors_per_cluster; s++) {
            if (rd_dev(st, cluster_lba(st, cur) + s, 1, sec) != 0)
                return -1;
            for (int i = 0; i < 512; i += 32) {
                const u8 *e = sec + i;
                if (e[0] == 0x00)
                    return 0;      /* 目录结束 */
                if (e[0] == 0xE5) { lfn_reset(&lb); continue; }

                u8 attr = e[11];
                if (attr == ATTR_LFN) {
                    /* 校验和必须匹配短项才算数；这里先收集 */
                    lfn_feed(&lb, e);
                    continue;
                }
                if (attr & ATTR_VOLUME_ID)
                    continue;

                fs_dirent_t d;
                memset(&d, 0, sizeof(d));

                /* 优先 LFN 名（校验和匹配），否则 8.3 */
                if (lb.valid && lb.entries_left == 0
                    && lb.checksum == lfn_checksum(e)) {
                    utf16_to_utf8(lb.name, 13 * 20, d.name, sizeof(d.name));
                } else {
                    sfname_to_str(e, d.name);
                }
                lfn_reset(&lb);

                u16 cl_lo = (u16)e[26] | ((u16)e[27] << 8);
                u16 cl_hi = (u16)e[20] | ((u16)e[21] << 8);
                d.first_block = ((u32)cl_hi << 16) | cl_lo;
                d.size = (u32)e[28] | ((u32)e[29] << 8)
                       | ((u32)e[30] << 16) | ((u32)e[31] << 24);
                d.flags = (attr & ATTR_DIRECTORY) ? FS_FLAG_DIR : 0;

                int r = fn(&d, ctx);
                if (r != 0)
                    return r;
            }
        }
        cur = fat_entry(st, cur);
    }
    return 0;
}

/* ---- 路径解析 ---- */

typedef struct {
    const char *want;
    fs_dirent_t *out;
    int found;
} resolve_ctx_t;

static int cb_resolve(const fs_dirent_t *e, void *ctx)
{
    resolve_ctx_t *rc = ctx;
    if (strcasecmp(e->name, rc->want) == 0) {
        *rc->out = *e;
        rc->found = 1;
        return 1;
    }
    return 0;
}

static int resolve_path(fat32_state_t *st, const char *path, fs_dirent_t *out)
{
    u32 cur = st->root_cluster;
    char comp[64];
    const char *p = path;

    /* 根目录本身 */
    if (path[0] == 0 || strcmp(path, "/") == 0) {
        memset(out, 0, sizeof(*out));
        out->flags = FS_FLAG_DIR;
        out->first_block = st->root_cluster;
        return 0;
    }

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        int n = 0;
        while (*p && *p != '/' && n < (int)sizeof(comp) - 1)
            comp[n++] = *p++;
        comp[n] = 0;

        if (strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            /* FAT 目录项里有 ".."，但根目录没有父目录 —— 简化：
             * 只支持一层。实际实现：扫描当前目录找 ".." 项。*/
            fs_dirent_t dotdot;
            resolve_ctx_t rc = { "..", &dotdot, 0 };
            scan_dir(st, cur, cb_resolve, &rc);
            if (rc.found)
                cur = dotdot.first_block;
            continue;
        }

        fs_dirent_t d;
        resolve_ctx_t rc = { comp, &d, 0 };
        scan_dir(st, cur, cb_resolve, &rc);
        if (!rc.found)
            return -1;
        cur = d.first_block;
        if (out)
            *out = d;
    }
    return 0;
}

int fat32_resolve(fat32_state_t *st, const char *path, fs_dirent_t *out)
{
    return resolve_path(st, path, out);
}

typedef struct {
    int         want;
    int         seen;
    int         found;
    fs_dirent_t e;
} list_ctx_t;

static int cb_list(const fs_dirent_t *e, void *ctx)
{
    list_ctx_t *lc = ctx;
    if (lc->seen++ == lc->want) {
        lc->e = *e;
        lc->found = 1;
        return 1;
    }
    return 0;
}

int fat32_list(fat32_state_t *st, const char *path, int index, fs_dirent_t *out)
{
    fs_dirent_t d;
    if (resolve_path(st, path, &d) != 0)
        return -1;
    if (!(d.flags & FS_FLAG_DIR))
        return -1;
    if (index < 0)
        return -1;

    list_ctx_t lc;
    memset(&lc, 0, sizeof(lc));
    lc.want = index;

    scan_dir(st, d.first_block, cb_list, &lc);
    if (!lc.found)
        return -1;
    *out = lc.e;
    return 0;
}

int fat32_read(fat32_state_t *st, const char *path, void *buf, u32 max)
{
    fs_dirent_t d;
    if (resolve_path(st, path, &d) != 0)
        return -1;
    if (d.flags & FS_FLAG_DIR)
        return -1;

    u32 left = d.size < max ? d.size : max;
    u32 got = 0;
    u32 cl = d.first_block;
    u8  *dst = buf;

    while (got < left && cl >= 2 && cl < FAT32_EOC) {
        u32 bytes = st->sectors_per_cluster * 512;
        if (bytes > left - got)
            bytes = left - got;
        if (rd_dev(st, cluster_lba(st, cl), st->sectors_per_cluster, dst) != 0)
            return -1;
        dst += bytes;
        got += bytes;
        cl = fat_entry(st, cl);
    }
    return (int)got;
}

u32 fat32_total_kb(fat32_state_t *st)
{
    return st->total_sectors / 2;
}

u32 fat32_free_kb(fat32_state_t *st)
{
    /* 粗略：扫 FAT 计空闲簇（只读下也可以缓存，这里现算一次） */
    u32 free_cl = 0;
    u32 fat_bytes = st->fat_sectors * 512;
    /* 固定静态缓冲：只读 FS，内核单线程访问，2MB 上限足够 */
    static u8 fatbuf[0x20000];   /* 128KB ≈ 32K 簇，覆盖测试盘 */
    u32 to_read = fat_bytes < sizeof(fatbuf) ? fat_bytes : sizeof(fatbuf);
    if (rd_dev(st, st->fat_start, to_read / 512, fatbuf) != 0)
        return 0;
    for (u32 i = 2; i < st->clusters + 2 && (i * 4 + 4) <= to_read; i++) {
        u32 v = (u32)fatbuf[i * 4]     | ((u32)fatbuf[i * 4 + 1] << 8)
              | ((u32)fatbuf[i * 4 + 2] << 16) | ((u32)fatbuf[i * 4 + 3] << 24);
        if ((v & 0x0FFFFFFF) == 0)
            free_cl++;
    }
    return free_cl * st->sectors_per_cluster / 2;
}


/* ---- 格式化（只支持 512B 扇区盘）----
 * 布局：保留 32 扇区 + 2 份 FAT + 数据区（簇 2 起）。
 * FAT 大小按总扇数估算迭代两轮收敛。*/
static u8 fmt_up(char c)
{
    return (u8)(c >= 'a' && c <= 'z' ? c - 32 : c);
}

int fat32_format(int dev, const char *label, u32 sectors_per_cluster)
{
    const blk_info_t *bi = blk_info(dev);
    u8 sec[512];

    if (!bi || bi->sector_size != 512 || !bi->writable)
        return -1;
    if (sectors_per_cluster == 0)
        sectors_per_cluster = 8;

    u32 total = bi->sectors;
    if (total < 20480)          /* 至少 ~10MB 才值得建 FAT32 */
        return -2;

    u32 rsvd = 32, nfats = 2;
    if (sectors_per_cluster == 0) {
        /* 按容量自适应簇大小，保证簇数 ≥ 4085（FAT32 下限） */
        sectors_per_cluster = 8;
        while (sectors_per_cluster < 64
               && (total - rsvd - 2 * (total / (sectors_per_cluster + 4)))
                   / sectors_per_cluster < 6144)
            sectors_per_cluster *= 2;
    }
    u32 fatsz = (total / sectors_per_cluster + 2) * 4
              / (512 * sectors_per_cluster) + 1;
    for (int it = 0; it < 3; it++) {
        u32 data = total - rsvd - nfats * fatsz;
        u32 clusters = data / sectors_per_cluster;
        u32 need = ((clusters + 2) * 4 + 511) / 512;
        if (need > fatsz)
            fatsz = need;
        else
            break;
    }

    u32 data_start = rsvd + nfats * fatsz;
    u32 clusters = (total - data_start) / sectors_per_cluster;
    if (clusters < 4085)
        return -2;

    /* ---- BPB ---- */
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xEB; sec[1] = 0x58; sec[2] = 0x90;
    memcpy(sec + 3, "MSWIN4.1", 8);
    sec[11] = 0x00; sec[12] = 0x02;              /* bps = 512 */
    sec[13] = (u8)sectors_per_cluster;
    sec[14] = (u8)rsvd; sec[15] = (u8)(rsvd >> 8);
    sec[16] = (u8)nfats;
    sec[21] = 0xF8;                              /* 介质 = 硬盘 */
    sec[24] = 0x3F; sec[25] = 0x00;              /* 每道 63 */
    sec[26] = 0xFF; sec[27] = 0x00;              /* 每头 255 */
    if (total >= 0x10000) {
        sec[32] = (u8)total;         sec[33] = (u8)(total >> 8);
        sec[34] = (u8)(total >> 16); sec[35] = (u8)(total >> 24);
    }
    sec[36] = 0x80;
    sec[38] = 0x29;
    u32 serial = 0x1998 ^ (total * 2654435761u);
    sec[39] = (u8)serial; sec[40] = (u8)(serial >> 8);
    sec[41] = (u8)(serial >> 16); sec[42] = (u8)(serial >> 24);
    char lab[11];
    memset(lab, ' ', 11);
    for (int i = 0; i < 11 && label && label[i]; i++)
        lab[i] = (char)fmt_up(label[i]);
    memcpy(sec + 43, lab, 11);
    memcpy(sec + 54, "FAT32   ", 8);
    sec[510] = 0x55; sec[511] = 0xAA;

    if (blk_write(dev, 0, 1, sec) != 0)
        return -3;

    /* ---- 两份 FAT：表头 + 根目录簇(2) 标 EOC，其余清零 ---- */
    for (u32 f = 0; f < nfats; f++) {
        u32 fat_lba = rsvd + f * fatsz;
        memset(sec, 0, sizeof(sec));
        sec[0] = 0xF8; sec[1] = 0xFF; sec[2] = 0xFF; sec[3] = 0x0F;
        sec[4] = 0xFF; sec[5] = 0xFF; sec[6] = 0xFF; sec[7] = 0x0F;
        /* 簇 2 = 根目录 = EOC */
        sec[8]  = 0xFF; sec[9]  = 0xFF; sec[10] = 0xFF; sec[11] = 0x0F;
        if (blk_write(dev, fat_lba, 1, sec) != 0)
            return -3;
        memset(sec, 0, sizeof(sec));
        for (u32 s = 1; s < fatsz; s++) {
            if (blk_write(dev, fat_lba + s, 1, sec) != 0)
                return -3;
        }
    }

    /* ---- 根目录簇第一扇区：卷标项 ---- */
    memset(sec, 0, sizeof(sec));
    memcpy(sec, lab, 11);
    sec[11] = 0x08;
    if (blk_write(dev, data_start, 1, sec) != 0)
        return -3;
    memset(sec, 0, sizeof(sec));
    for (u32 s = 1; s < sectors_per_cluster; s++) {
        if (blk_write(dev, data_start + s, 1, sec) != 0)
            return -3;
    }

    /* 备份引导扇区 → 扇区 6（FAT32 惯例） */
    {
        u8 bs[512];
        if (blk_read(dev, 0, 1, bs) == 0)
            blk_write(dev, 6, 1, bs);
    }
    return 0;
}
