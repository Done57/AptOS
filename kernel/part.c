/*
 * MBR 分区支持。
 *
 * 主硬盘注册后扫描 MBR 分区表，把每个分区再注册成独立块设备
 * （hd0p1、hd0p2…）。分区设备的读写直接落在父设备上，只是
 * LBA 平移了分区起始扇区 —— 对上层（fs.c 的探测、TUI）来说
 * 它就是一个普通块设备，不需要知道分区这回事。
 *
 * 分区表项（16 字节）：
 *   +0  状态（0x80 可引导）
 *   +4  分区类型（0x0B/0x0C FAT32, 0x07 NTFS/exFAT, 0x83 Linux）
 *   +8  起始 LBA（小端）
 *   +12 总扇区数
 */
#include "blkdev.h"
#include "klog.h"
#include "util.h"

#define MBR_LBA        0
#define PART_TABLE_OFF 446
#define PART_ENTRY_SZ  16
#define MAX_PARTS      4

typedef struct {
    int parent;
    u32 start;
    u32 count;
} part_slot_t;

static part_slot_t parts[BLK_MAX];
static int nparts;

/* 分区设备的读回调：dev 是分区自己，LBA 平移到父设备 */
static int part_rd(int dev, u32 lba, u32 count, void *buf)
{
    if (dev < 0 || dev >= nparts)
        return -1;
    part_slot_t *p = &parts[dev];
    return blk_read(p->parent, p->start + lba, count, buf);
}

static int part_wr(int dev, u32 lba, u32 count, const void *buf)
{
    if (dev < 0 || dev >= nparts)
        return -1;
    part_slot_t *p = &parts[dev];
    return blk_write(p->parent, p->start + lba, count, buf);
}

static const char *part_type_name(u8 t)
{
    switch (t) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/exFAT";
    case 0x83: return "Linux";
    case 0xEE: case 0xEF: return "GPT";
    default:   return "unknown";
    }
}

void part_scan_all(void)
{
    nparts = 0;
    memset(parts, 0, sizeof(parts));

    /* 只扫 512 字节扇区的硬盘 */
    for (int d = 0; d < blk_count(); d++) {
        const blk_info_t *bi = blk_info(d);
        if (!bi || bi->sector_size != 512 || bi->type != BLK_HDD)
            continue;

        u8 mbr[512];
        if (blk_read(d, MBR_LBA, 1, mbr) != 0)
            continue;
        if (mbr[510] != 0x55 || mbr[511] != 0xAA)
            continue;   /* 没有 MBR 签名：整盘就是一个文件系统或裸盘 */

        /* FAT32/NTFS 的引导扇区末尾同样有 0x55AA，但它们是文件系统
         * 而不是分区表。按 OEM/卷标标志把它们排除，否则会把引导
         * 代码当成分区表项读出一堆鬼分区。*/
        if (memcmp(mbr + 3, "NTFS    ", 8) == 0)
            continue;
        if (memcmp(mbr + 82, "FAT32   ", 8) == 0)
            continue;
        if (memcmp(mbr + 54, "FAT", 3) == 0 &&
            (memcmp(mbr + 59, "12", 2) == 0 || memcmp(mbr + 59, "16", 2) == 0))
            continue;

        /* 分区项必须自洽：起点在盘内、终点不越界 */
        int plausible = 0;
        for (int i = 0; i < MAX_PARTS; i++) {
            const u8 *e = mbr + PART_TABLE_OFF + i * PART_ENTRY_SZ;
            u32 st = (u32)e[8]  | ((u32)e[9] << 8) |
                     ((u32)e[10] << 16) | ((u32)e[11] << 24);
            u32 cn = (u32)e[12] | ((u32)e[13] << 8) |
                     ((u32)e[14] << 16) | ((u32)e[15] << 24);
            if (e[4] != 0 && cn != 0 && st >= 1 &&
                (u64)st + cn <= (u64)bi->sectors)
                plausible = 1;
        }
        if (!plausible)
            continue;

        for (int i = 0; i < MAX_PARTS; i++) {
            const u8 *e = mbr + PART_TABLE_OFF + i * PART_ENTRY_SZ;
            u8  ptype  = e[4];
            u32 start  = (u32)e[8]  | ((u32)e[9] << 8) |
                         ((u32)e[10] << 16) | ((u32)e[11] << 24);
            u32 count  = (u32)e[12] | ((u32)e[13] << 8) |
                         ((u32)e[14] << 16) | ((u32)e[15] << 24);

            if (ptype == 0 || count == 0)
                continue;
            if (nparts >= BLK_MAX)
                break;

            char name[8];
            snprintf(name, sizeof(name), "%sp%d", bi->name, i + 1);

            int dev = blk_register(name, BLK_HDD, count, 512,
                                   bi->removable, bi->writable,
                                   part_type_name(ptype), part_rd, part_wr);
            if (dev >= 0) {
                parts[dev].parent = d;
                parts[dev].start  = start;
                parts[dev].count  = count;
                if (dev >= nparts)
                    nparts = dev + 1;
                kprintf("[part] %s: type %s, %u sectors (start %u)\n",
                        name, part_type_name(ptype), count, start);
            }
        }
    }
}


/* ---- 创建单个主分区（MBR，整盘）----
 * 把整盘划成一个主分区（类型由 fs_format_type 决定后写 0x0B/0x07/0x83），
 * 写 MBR 签名与表项，然后按 part_scan_all 相同的方式注册分区设备。
 * 返回分区设备名（hdXp1）。已有分区表时返回 -6 要求先确认覆盖。*/
int part_create_single(int parent, u8 ptype, char *out_name, int out_max)
{
    const blk_info_t *bi = blk_info(parent);
    u8 mbr[512];

    if (!bi || bi->sector_size != 512 || !bi->writable)
        return -1;
    if (bi->sectors < 2080)          /* 至少 ~1MB */
        return -2;

    if (blk_read(parent, 0, 1, mbr) != 0)
        return -3;

    /* 有现成分区表就拒绝（覆盖需要用户显式再按一次"继续"） */
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        const u8 *e0 = mbr + PART_TABLE_OFF;
        if (e0[4] != 0 || e0[12] != 0)
            return -6;
    }

    memset(mbr, 0, sizeof(mbr));
    /* 引导代码区放个标识别删 */
    memcpy(mbr, "APTOSMBR", 8);
    u8 *e = mbr + PART_TABLE_OFF;
    e[0] = 0x80;                     /* 可引导 */
    e[4] = ptype;
    /* CHS 字段留给 BIOS 兼容，全 0xFE/0xFF 填充即可，QEMU 不校验 */
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
    u32 start = 2048;                /* 1MB 对齐，Windows 惯例 */
    u32 count = bi->sectors - start;
    e[8]  = (u8)start;        e[9]  = (u8)(start >> 8);
    e[10] = (u8)(start >> 16); e[11] = (u8)(start >> 24);
    e[12] = (u8)count;        e[13] = (u8)(count >> 8);
    e[14] = (u8)(count >> 16); e[15] = (u8)(count >> 24);
    mbr[510] = 0x55; mbr[511] = 0xAA;

    if (blk_write(parent, 0, 1, mbr) != 0)
        return -3;

    /* 立刻注册分区设备（同 part_scan_all 的形状） */
    if (nparts >= BLK_MAX)
        return -5;
    char name[8];
    snprintf(name, sizeof(name), "%sp1", bi->name);
    int dev = blk_register(name, BLK_HDD, count, 512,
                           bi->removable, 1, part_type_name(ptype),
                           part_rd, part_wr);
    if (dev < 0)
        return -5;
    parts[dev].parent = parent;
    parts[dev].start  = start;
    parts[dev].count  = count;
    if (dev >= nparts)
        nparts = dev + 1;
    if (out_name && out_max > 0)
        snprintf(out_name, (u32)out_max, "%s", name);
    kprintf("[part] created %s: type %s, %u sectors (start %u)\n",
            name, part_type_name(ptype), count, start);
    return 0;
}
