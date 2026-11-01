/*
 * NTFS 只读驱动。
 *
 * NTFS 的一切都是文件：$MFT 自身也是一条 MFT 记录，所有元数据
 * （$Volume、$Bitmap、$AttrDef…）都靠 MFT 记录描述。读盘三步走：
 *   1. 引导扇区给出每簇字节数、$MFT 起始簇、记录大小；
 *   2. 读 $MFT 记录 0 的 $DATA 属性，解出它自己的 run list，
 *      于是"第 N 条 MFT 记录"就能定位了；
 *   3. 目录的数据就是 $INDEX_ROOT（常驻，小目录）与
 *      $INDEX_ALLOCATION（非常驻，大目录）组成的 B+ 树，
 *      遍历其中的 $FILE_NAME 索引项即可列出子项。
 *
 * 两个必须做对、做错就静默读歪的细节：
 *   - **更新序列（USA/fixup）**：每条 MFT 记录与 INDX 记录的最后
 *     两个字节被替换成序列号，读出来必须用 USA 数组还原，
 *     否则每个 512 字节扇区尾部两字节都是错的。
 *   - **run list 的偏移是相对上一段的增量，且是有符号数**，
 *     需要按字节数做符号扩展。
 */
#include "ntfs.h"
#include "blkdev.h"
#include "klog.h"
#include "util.h"

/* 属性类型 */
#define AT_STANDARD_INFO   0x10
#define AT_FILE_NAME       0x30
#define AT_VOLUME_NAME     0x60
#define AT_VOLUME_INFO     0x70
#define AT_DATA            0x80
#define AT_INDEX_ROOT      0x90
#define AT_INDEX_ALLOC     0xA0
#define AT_END             0xFFFFFFFF

#define MFT_ROOT_RECORD    5u    /* 根目录固定是第 5 条记录 */

/* $FILE_NAME 里的"文件属性"字段。目录位是 0x10000000 ——
 * 别用 0x2，那是"隐藏"，拿它判目录会把隐藏文件全当成目录。*/
#define NF_ATTR_DIRECTORY  0x10000000u

static int fname_flags_is_dir(const u8 *v)
{
    u32 f = (u32)v[0x30] | ((u32)v[0x31] << 8)
          | ((u32)v[0x32] << 16) | ((u32)v[0x33] << 24);
    return (f & NF_ATTR_DIRECTORY) != 0;
}

static u8  mft_rec[4096];
static u8  idx_rec[8192];
static u8  clus_buf[65536];

/* ---- 底层读 ---- */
static int rd_bytes(ntfs_state_t *st, u64 off, u32 len, void *buf)
{
    u64 lba = off / st->bytes_per_sector;
    u32 cnt = (len + st->bytes_per_sector - 1) / st->bytes_per_sector;
    return blk_read(st->dev, (u32)lba, cnt, buf);
}

static int rd_cluster(ntfs_state_t *st, u64 lcn, void *buf)
{
    return blk_read(st->dev, (u32)(lcn * st->sectors_per_cluster),
                    st->sectors_per_cluster, buf);
}

int ntfs_bind(ntfs_state_t *st, int dev)
{
    const blk_info_t *bi = blk_info(dev);
    u8 bs[512];

    memset(st, 0, sizeof(*st));
    if (!bi || bi->sector_size != 512)
        return -1;
    if (blk_read(dev, 0, 1, bs) != 0)
        return -1;
    if (memcmp(bs + 3, "NTFS    ", 8) != 0)
        return -1;

    st->dev = dev;
    st->bytes_per_sector    = (u32)bs[11] | ((u32)bs[12] << 8);
    st->sectors_per_cluster = bs[13];
    st->total_sectors       = (u64)bs[40] | ((u64)bs[41] << 8)
                            | ((u64)bs[42] << 16) | ((u64)bs[43] << 24)
                            | ((u64)bs[44] << 32) | ((u64)bs[45] << 40)
                            | ((u64)bs[46] << 48) | ((u64)bs[47] << 56);
    st->mft_lcn             = (u64)bs[48] | ((u64)bs[49] << 8)
                            | ((u64)bs[50] << 16) | ((u64)bs[51] << 24)
                            | ((u64)bs[52] << 32) | ((u64)bs[53] << 40)
                            | ((u64)bs[54] << 48) | ((u64)bs[55] << 56);
    st->mftmirr_lcn         = (u64)bs[56] | ((u64)bs[57] << 8)
                            | ((u64)bs[58] << 16) | ((u64)bs[59] << 24)
                            | ((u64)bs[60] << 32) | ((u64)bs[61] << 40)
                            | ((u64)bs[62] << 48) | ((u64)bs[63] << 56);

    if (st->bytes_per_sector < 256 || st->bytes_per_sector > 4096)
        return -1;
    if (st->sectors_per_cluster == 0 || st->mft_lcn == 0)
        return -1;
    st->cluster_size = st->bytes_per_sector * st->sectors_per_cluster;
    if (st->cluster_size > sizeof(clus_buf))
        return -1;

    /* 每条 MFT 记录的大小：正数=簇数，负数=2^(-n) 字节 */
    s8 cpm = (s8)bs[64];
    if (cpm > 0)
        st->mft_record_size = (u32)cpm * st->cluster_size;
    else
        st->mft_record_size = 1u << (u32)(-cpm);

    s8 cpi = (s8)bs[68];
    if (cpi > 0)
        st->index_record_size = (u32)cpi * st->cluster_size;
    else
        st->index_record_size = 1u << (u32)(-cpi);

    if (st->mft_record_size < 256 || st->mft_record_size > sizeof(mft_rec))
        return -1;
    if (st->index_record_size > sizeof(idx_rec))
        st->index_record_size = sizeof(idx_rec);

    strcpy(st->label, "NTFS");
    st->mounted = 1;
    return 0;
}

/* ---- 更新序列（fixup）还原 ---- */
static void apply_fixup(u8 *rec, u32 rec_size)
{
    u16 usa_off   = (u16)rec[4] | ((u16)rec[5] << 8);
    u16 usa_count = (u16)rec[6] | ((u16)rec[7] << 8);
    if (usa_count < 2 || (u32)usa_off + (u32)usa_count * 2 > rec_size)
        return;

    u16 usn = (u16)rec[usa_off] | ((u16)rec[usa_off + 1] << 8);
    u32 nsec = rec_size / 512;

    for (u32 i = 0; i < nsec && (i + 1) < usa_count; i++) {
        u8 *tail = rec + (i + 1) * 512 - 2;
        u16 cur = (u16)tail[0] | ((u16)tail[1] << 8);
        if (cur != usn)
            return;     /* 序列号不符：记录已损坏，拒绝还原 */
        u16 orig = (u16)rec[usa_off + (i + 1) * 2]
                 | ((u16)rec[usa_off + (i + 1) * 2 + 1] << 8);
        tail[0] = (u8)(orig & 0xFF);
        tail[1] = (u8)(orig >> 8);
    }
}

/* ---- run list 解码 ----
 * 输出段表：每段 { lcn, clusters }，最多 max 段。
 * 返回段数（0 表示空/失败）。*/
typedef struct { u64 lcn; u64 clusters; } run_t;

static int decode_runs(const u8 *rl, u32 rl_len, run_t *out, int max,
                       u64 *total_clusters)
{
    int n = 0;
    s64 lcn = 0;
    u32 i = 0;
    u64 total = 0;

    while (i < rl_len && rl[i] != 0) {
        u8 hdr = rl[i++];
        int lenb = hdr & 0x0F;
        int offb = (hdr >> 4) & 0x0F;
        if (lenb == 0 || i + lenb + offb > rl_len)
            break;

        u64 len = 0;
        for (int k = 0; k < lenb; k++)
            len |= (u64)rl[i + k] << (8 * k);
        i += lenb;

        s64 off = 0;
        for (int k = 0; k < offb; k++)
            off |= (s64)(u64)rl[i + k] << (8 * k);
        /* 符号扩展：offb 字节的最高位是符号位 */
        if (offb > 0 && offb < 8 && (rl[i + offb - 1] & 0x80))
            off |= ~(((s64)1 << (8 * offb)) - 1);
        i += offb;

        lcn += off;
        total += len;
        if (n < max) {
            out[n].lcn = (u64)lcn;
            out[n].clusters = len;
            n++;
        }
    }
    if (total_clusters)
        *total_clusters = total;
    return n;
}

/* ---- 属性遍历 ---- */
typedef struct {
    u32 type;
    int non_resident;
    const u8 *value;      /* 常驻：值指针 */
    u32 value_len;
    u64 real_size;        /* 非常驻：真实大小 */
    const u8 *runs;       /* 非常驻：run list 指针 */
    u32 runs_len;
} ntfs_attr_t;

/* 在记录里找指定类型（且未命名）的属性；从 attr_off 开始 */
static int find_attr(const u8 *rec, u32 attr_off, u32 type, ntfs_attr_t *out)
{
    u32 off = attr_off;
    u32 rec_size = (u32)rec[0x1C] | ((u32)rec[0x1D] << 8)
                 | ((u32)rec[0x1E] << 16) | ((u32)rec[0x1F] << 24);

    while (off + 8 <= rec_size) {
        u32 atype = (u32)rec[off] | ((u32)rec[off+1] << 8)
                  | ((u32)rec[off+2] << 16) | ((u32)rec[off+3] << 24);
        u32 alen  = (u32)rec[off+4] | ((u32)rec[off+5] << 8)
                  | ((u32)rec[off+6] << 16) | ((u32)rec[off+7] << 24);
        if (atype == AT_END || alen < 8)
            break;

        u8  nres  = rec[off + 8];
        u8  nlen  = rec[off + 9];
        u16 noff  = (u16)rec[off+10] | ((u16)rec[off+11] << 8);

        if (atype == type && nlen == 0) {
            memset(out, 0, sizeof(*out));
            out->type = atype;
            out->non_resident = nres;
            if (!nres) {
                u32 vlen = (u32)rec[off+16] | ((u32)rec[off+17] << 8)
                         | ((u32)rec[off+18] << 16) | ((u32)rec[off+19] << 24);
                u16 voff = (u16)rec[off+20] | ((u16)rec[off+21] << 8);
                out->value = rec + off + voff;
                out->value_len = vlen;
                out->real_size = vlen;
            } else {
                u16 roff = (u16)rec[off+32] | ((u16)rec[off+33] << 8);
                out->real_size = (u64)rec[off+48] | ((u64)rec[off+49] << 8)
                               | ((u64)rec[off+50] << 16) | ((u64)rec[off+51] << 24)
                               | ((u64)rec[off+52] << 32) | ((u64)rec[off+53] << 40)
                               | ((u64)rec[off+54] << 48) | ((u64)rec[off+55] << 56);
                out->runs = rec + off + roff;
                out->runs_len = alen > roff ? alen - roff : 0;
            }
            return 0;
        }
        (void)noff;
        off += alen;
    }
    return -1;
}

#define ATTR_OFF(rec) ((u32)(rec)[0x14] | ((u32)(rec)[0x15] << 8))

/* ---- 读 MFT 记录 ----
 * $MFT 自身的位置靠"记录 0 的 $DATA run list"确定；其余记录
 * 按记录大小线性排布在 $MFT 数据里。*/
static run_t mft_runs[64];
static int    mft_run_count;
static u64    mft_data_bytes;

static int load_mft_runs(ntfs_state_t *st)
{
    u64 off = st->mft_lcn * st->cluster_size;
    if (rd_bytes(st, off, st->mft_record_size, mft_rec) != 0)
        return -1;
    if (memcmp(mft_rec, "FILE", 4) != 0)
        return -1;
    apply_fixup(mft_rec, st->mft_record_size);

    ntfs_attr_t a;
    if (find_attr(mft_rec, ATTR_OFF(mft_rec), AT_DATA, &a) != 0)
        return -1;
    if (!a.non_resident)
        return -1;

    mft_run_count = decode_runs(a.runs, a.runs_len, mft_runs, 64, &mft_data_bytes);
    return mft_run_count > 0 ? 0 : -1;
}

/* 把 $MFT 数据内的字节偏移映射到设备字节偏移 */
static int mft_offset_to_dev(ntfs_state_t *st, u64 data_off, u64 *dev_off)
{
    u64 acc = 0;
    for (int i = 0; i < mft_run_count; i++) {
        u64 seg = mft_runs[i].clusters * st->cluster_size;
        if (data_off < acc + seg) {
            *dev_off = mft_runs[i].lcn * st->cluster_size + (data_off - acc);
            return 0;
        }
        acc += seg;
    }
    return -1;
}

/* 读第 rec_no 条 MFT 记录（已还原 fixup） */
static int read_mft_record(ntfs_state_t *st, u64 rec_no, u8 *out)
{
    u64 data_off = rec_no * st->mft_record_size;
    u64 dev_off;

    if (mft_offset_to_dev(st, data_off, &dev_off) != 0)
        return -1;
    if (rd_bytes(st, dev_off, st->mft_record_size, out) != 0)
        return -1;
    if (memcmp(out, "FILE", 4) != 0)
        return -1;
    apply_fixup(out, st->mft_record_size);
    return 0;
}

/* ---- 从 run list 读数据 ---- */
static int read_from_runs(ntfs_state_t *st, const run_t *runs, int nruns,
                          u64 start_byte, u32 len, void *buf)
{
    u8 *dst = buf;
    u64 skip = start_byte;
    u32 got = 0;

    for (int i = 0; i < nruns && got < len; i++) {
        u64 seg = runs[i].clusters * st->cluster_size;
        if (skip >= seg) {
            skip -= seg;
            continue;
        }
        u64 pos = skip;
        skip = 0;
        while (pos < seg && got < len) {
            u64 lcn = runs[i].lcn + pos / st->cluster_size;
            u32 in_cl = (u32)(pos % st->cluster_size);
            if (rd_cluster(st, lcn, clus_buf) != 0)
                return -1;
            u32 n = st->cluster_size - in_cl;
            if (n > len - got)
                n = len - got;
            memcpy(dst + got, clus_buf + in_cl, n);
            got += n;
            pos += n;
        }
    }
    return (int)got;
}

/* 读某个属性的全部（或前 len 字节）内容 */
static int read_attr(ntfs_state_t *st, const ntfs_attr_t *a, u64 off,
                     u32 len, void *buf)
{
    if (!a->non_resident) {
        if (off >= a->value_len)
            return 0;
        u32 n = a->value_len - (u32)off;
        if (n > len)
            n = len;
        memcpy(buf, a->value + off, n);
        return (int)n;
    }
    run_t runs[64];
    u64 total = 0;
    int nr = decode_runs(a->runs, a->runs_len, runs, 64, &total);
    if (nr <= 0)
        return -1;
    return read_from_runs(st, runs, nr, off, len, buf);
}

/* ---- 文件名（$FILE_NAME 属性）---- */
static void utf16le_to_utf8(const u8 *src, int nchars, char *out, int outsz)
{
    int n = 0;
    for (int i = 0; i < nchars; i++) {
        u16 c = (u16)src[i*2] | ((u16)src[i*2+1] << 8);
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
}

/* 取记录的首选 $FILE_NAME（Win32 命名空间优先） */
static int record_name(const u8 *rec, char *out, int outsz, u64 *parent,
                       u64 *real_size, int *is_dir)
{
    u32 off = ATTR_OFF(rec);
    u32 rec_size = (u32)rec[0x1C] | ((u32)rec[0x1D] << 8)
                 | ((u32)rec[0x1E] << 16) | ((u32)rec[0x1F] << 24);
    int best = -1;
    const u8 *bestv = NULL;

    while (off + 8 <= rec_size) {
        u32 atype = (u32)rec[off] | ((u32)rec[off+1] << 8)
                  | ((u32)rec[off+2] << 16) | ((u32)rec[off+3] << 24);
        u32 alen  = (u32)rec[off+4] | ((u32)rec[off+5] << 8)
                  | ((u32)rec[off+6] << 16) | ((u32)rec[off+7] << 24);
        if (atype == AT_END || alen < 8)
            break;
        if (atype == AT_FILE_NAME && rec[off+8] == 0) {
            u16 voff = (u16)rec[off+20] | ((u16)rec[off+21] << 8);
            const u8 *v = rec + off + voff;
            u8 ns = v[0x39];
            /* 0 = POSIX, 1 = Win32, 2 = DOS, 3 = Win32&DOS */
            int score = (ns == 1 || ns == 3) ? 2 : (ns == 0 ? 1 : 0);
            if (score > best) {
                best = score;
                bestv = v;
            }
        }
        off += alen;
    }
    if (!bestv)
        return -1;

    if (parent) {
        *parent = (u64)bestv[0] | ((u64)bestv[1] << 8) | ((u64)bestv[2] << 16)
                | ((u64)bestv[3] << 24) | ((u64)bestv[4] << 32)
                | ((u64)bestv[5] << 40) | ((u64)bestv[6] << 48)
                | ((u64)bestv[7] << 56);
        *parent &= 0x0000FFFFFFFFFFFFull;
    }
    if (real_size) {
        *real_size = (u64)bestv[0x28] | ((u64)bestv[0x29] << 8)
                   | ((u64)bestv[0x2A] << 16) | ((u64)bestv[0x2B] << 24)
                   | ((u64)bestv[0x2C] << 32) | ((u64)bestv[0x2D] << 40)
                   | ((u64)bestv[0x2E] << 48) | ((u64)bestv[0x2F] << 56);
    }
    if (is_dir)
        *is_dir = fname_flags_is_dir(bestv);

    int nlen = bestv[0x38];
    if (nlen > 255)
        nlen = 255;
    utf16le_to_utf8(bestv + 0x3A, nlen, out, outsz);
    return 0;
}

/* ---- 目录索引遍历 ---- */

typedef int (*idx_cb)(u64 mft_ref, const char *name, u64 size, int is_dir,
                      void *ctx);

/* 遍历一段索引项区域（$INDEX_ROOT 的值或一条 INDX 记录） */
static int walk_index_entries(const u8 *hdr_base, u32 alloc_size,
                              idx_cb fn, void *ctx)
{
    u32 entries_off = (u32)hdr_base[0] | ((u32)hdr_base[1] << 8)
                    | ((u32)hdr_base[2] << 16) | ((u32)hdr_base[3] << 24);
    u32 entries_sz  = (u32)hdr_base[4] | ((u32)hdr_base[5] << 8)
                    | ((u32)hdr_base[6] << 16) | ((u32)hdr_base[7] << 24);
    (void)alloc_size;

    u32 off = entries_off;
    u32 end = entries_off + entries_sz;

    while (off + 0x10 <= end) {
        const u8 *e = hdr_base + off;
        u64 ref = (u64)e[0] | ((u64)e[1] << 8) | ((u64)e[2] << 16)
                | ((u64)e[3] << 24) | ((u64)e[4] << 32) | ((u64)e[5] << 40);
        u16 elen = (u16)e[8] | ((u16)e[9] << 8);
        u16 flags = (u16)e[12] | ((u16)e[13] << 8);

        if (elen < 0x10)
            break;
        if (flags & 0x2)   /* 末项：只有哨兵，没有文件名 */
            break;
        if (flags & 0x1) { /* 有子节点：本驱动不做 B 树下降，跳过 */ }

        /* $FILE_NAME 紧跟在 0x10 之后 */
        const u8 *fname = e + 0x10;
        int nlen = fname[0x38];
        if (nlen > 255) nlen = 255;

        char name[40];
        utf16le_to_utf8(fname + 0x3A, nlen, name, sizeof(name));
        u64 size = (u64)fname[0x28] | ((u64)fname[0x29] << 8)
                 | ((u64)fname[0x2A] << 16) | ((u64)fname[0x2B] << 24)
                 | ((u64)fname[0x2C] << 32) | ((u64)fname[0x2D] << 40);
        int isdir = fname_flags_is_dir(fname);

        if (name[0] && strcmp(name, ".") != 0)
            if (fn(ref, name, size, isdir, ctx) != 0)
                return 1;

        off += elen;
    }
    return 0;
}

/* 列表收集上下文 */
typedef struct {
    int want, seen, found;
    fs_dirent_t e;
} ntfs_list_ctx;

static int cb_collect(u64 ref, const char *name, u64 size, int isdir, void *ctx)
{
    ntfs_list_ctx *lc = ctx;
    if (lc->seen++ == lc->want) {
        memset(&lc->e, 0, sizeof(lc->e));
        strncpy(lc->e.name, name, sizeof(lc->e.name) - 1);
        lc->e.size = (u32)size;
        lc->e.first_block = (u32)ref;
        lc->e.flags = isdir ? FS_FLAG_DIR : 0;
        lc->found = 1;
        return 1;
    }
    return 0;
}

typedef struct {
    const char *want;
    u64 ref;
    u64 size;
    int isdir, found;
} ntfs_find_ctx;

static int cb_find(u64 ref, const char *name, u64 size, int isdir, void *ctx)
{
    ntfs_find_ctx *fc = ctx;
    if (strcasecmp(name, fc->want) == 0) {
        fc->ref = ref;
        fc->size = size;
        fc->isdir = isdir;
        fc->found = 1;
        return 1;
    }
    return 0;
}

/* 遍历目录记录的全部索引（$INDEX_ROOT + 所有 $INDEX_ALLOCATION 记录） */
static int walk_dir(ntfs_state_t *st, u64 dir_rec, idx_cb fn, void *ctx)
{
    if (read_mft_record(st, dir_rec, mft_rec) != 0)
        return -1;

    ntfs_attr_t root_a;
    if (find_attr(mft_rec, ATTR_OFF(mft_rec), AT_INDEX_ROOT, &root_a) != 0)
        return -1;

    /* $INDEX_ROOT 值布局：0x00 属性类型 0x04 排序规则 0x08 块大小
     * 0x0C 每记录簇数，索引头从 0x10 开始 */
    const u8 *hdr = root_a.value + 0x10;
    /* 返回 1 表示"本阶段停止"，不是"全部完成"—— 列表收集要
     * 跨越 $INDEX_ROOT 与 $INDEX_ALLOCATION 两个阶段累计。*/
    (void)walk_index_entries(hdr, 0, fn, ctx);

    /* 大目录：$INDEX_ALLOCATION + $BITMAP */
    ntfs_attr_t alloc_a, bmp_a;
    if (find_attr(mft_rec, ATTR_OFF(mft_rec), AT_INDEX_ALLOC, &alloc_a) != 0)
        return 0;   /* 小目录，只有根节点，正常 */

    int have_bmp = (find_attr(mft_rec, ATTR_OFF(mft_rec), 0xB0, &bmp_a) == 0);


    run_t runs[64];
    u64 total = 0;
    int nr = decode_runs(alloc_a.runs, alloc_a.runs_len, runs, 64, &total);
    if (nr <= 0)
        return 0;


    u64 nrec = alloc_a.real_size / st->index_record_size;
    for (u64 i = 0; i < nrec; i++) {
        if (have_bmp) {
            u8 bitbyte;
            if (read_attr(st, &bmp_a, i / 8, 1, &bitbyte) == 1) {
                if (!((bitbyte >> (i % 8)) & 1))
                    continue;   /* 该索引记录未使用 */
            }
        }
        if (read_attr(st, &alloc_a, i * st->index_record_size,
                      st->index_record_size, idx_rec) != (int)st->index_record_size)
            break;
        if (memcmp(idx_rec, "INDX", 4) != 0) {
            continue;
        }
        apply_fixup(idx_rec, st->index_record_size);

        /* INDX: 0x00 "INDX", 0x04 USA, 0x08 LSN, 0x10 VCN,
         * 索引头在 0x18 */
        (void)walk_index_entries(idx_rec + 0x18, 0, fn, ctx);
    }
    return 0;
}

/* ---- 路径解析 ---- */
static int resolve_rec(ntfs_state_t *st, const char *path, u64 *out_rec,
                       u64 *out_size, int *out_isdir)
{
    u64 cur = MFT_ROOT_RECORD;

    if (path[0] == 0 || strcmp(path, "/") == 0) {
        if (out_rec) *out_rec = cur;
        if (out_size) *out_size = 0;
        if (out_isdir) *out_isdir = 1;
        return 0;
    }

    const char *p = path;
    char comp[80];

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
            u64 parent = 0;
            if (read_mft_record(st, cur, mft_rec) == 0)
                record_name(mft_rec, comp, 1, &parent, NULL, NULL);
            (void)parent;
            if (parent >= 5)
                cur = parent;
            continue;
        }

        ntfs_find_ctx fc;
        memset(&fc, 0, sizeof(fc));
        fc.want = comp;
        if (walk_dir(st, cur, cb_find, &fc) != 0 || !fc.found)
            return -1;
        cur = fc.ref;
        if (out_size) *out_size = fc.size;
        if (out_isdir) *out_isdir = fc.isdir;
    }

    if (out_rec) *out_rec = cur;
    return 0;
}

int ntfs_resolve(ntfs_state_t *st, const char *path, fs_dirent_t *out)
{
    u64 rec, size = 0;
    int isdir = 0;

    if (load_mft_runs(st) != 0)
        return -1;
    if (resolve_rec(st, path, &rec, &size, &isdir) != 0)
        return -1;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->size = (u32)size;
        out->first_block = (u32)rec;
        out->flags = isdir ? FS_FLAG_DIR : 0;
        const char *b = strrchr(path, '/');
        strncpy(out->name, b ? b + 1 : path, sizeof(out->name) - 1);
    }
    return 0;
}

int ntfs_list(ntfs_state_t *st, const char *path, int index, fs_dirent_t *out)
{
    u64 rec, size = 0;
    int isdir = 0;

    if (load_mft_runs(st) != 0)
        return -1;
    if (resolve_rec(st, path, &rec, &size, &isdir) != 0)
        return -1;
    if (!isdir)
        return -1;

    ntfs_list_ctx lc;
    memset(&lc, 0, sizeof(lc));
    lc.want = index;

    walk_dir(st, rec, cb_collect, &lc);
    if (!lc.found)
        return -1;
    *out = lc.e;
    return 0;
}

int ntfs_read(ntfs_state_t *st, const char *path, void *buf, u32 max)
{
    u64 rec, size = 0;
    int isdir = 0;

    if (load_mft_runs(st) != 0)
        return -1;
    if (resolve_rec(st, path, &rec, &size, &isdir) != 0)
        return -1;
    if (isdir)
        return -1;
    if (read_mft_record(st, rec, mft_rec) != 0)
        return -1;

    ntfs_attr_t a;
    if (find_attr(mft_rec, ATTR_OFF(mft_rec), AT_DATA, &a) != 0)
        return -1;

    u64 real = a.non_resident ? a.real_size : a.value_len;
    u32 want = (real < max) ? (u32)real : max;
    int got = read_attr(st, &a, 0, want, buf);
    return got;
}

u32 ntfs_total_kb(ntfs_state_t *st)
{
    return (u32)((st->total_sectors * st->bytes_per_sector) / 1024);
}


/* ---- 小端写字节 helper（格式化骨架用） ---- */

/* ---- NTFS 空卷骨架 ----
 * 引导扇区（OEM "NTFS    "）+ 簇 4 处 32 簇 $MFT 区：记录 0 自举
 * （$DATA 非常驻 run：簇 4 起 32 簇），其余记录全零。
 * 只读驱动可挂载成空卷。*/
int ntfs_format_empty(int dev, const char *label)
{
    const blk_info_t *bi = blk_info(dev);
    u8 sec[512];

    if (!bi || bi->sector_size != 512 || !bi->writable)
        return -1;
    if (bi->sectors < 4096)
        return -2;

    u32 cluster = 4096;              /* 4KB 簇 = 8 扇区 */
    u32 mft_cluster = 4;

    /* 引导扇区 */
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xEB; sec[1] = 0x52; sec[2] = 0x90;
    memcpy(sec + 3, "NTFS    ", 8);
    sec[11] = 0x00; sec[12] = 0x02;          /* bps 512 */
    sec[13] = 0x08;                          /* 每簇 8 扇区 */
    sec[21] = 0xF8;
    fmt_le64(sec + 0x28, (u64)bi->sectors / 8);  /* 总簇数 */
    fmt_le64(sec + 0x30, mft_cluster);           /* $MFT 簇号 */
    fmt_le64(sec + 0x38, mft_cluster + 32);      /* $MFTMirr */
    sec[0x40] = (u8)(-12);                       /* MFT 记录 4096B */
    sec[0x44] = (u8)(-12);                       /* 索引记录 4096B */
    memcpy(sec + 0x48, "APTOSFMT", 8);           /* 序列号占位 */
    u32 llen = 0;
    while (label[llen] && llen < 16) {
        sec[0x4C + llen * 2] = (u8)label[llen];
        llen++;
    }
    sec[510] = 0x55; sec[511] = 0xAA;
    if (blk_write(dev, 0, 1, sec) != 0)
        return -3;

    /* $MFT 区：32 簇 = 128KB，全零，再填记录 0 */
    static u8 mft[8 * 4096];   /* 只放自举记录 0 */
    memset(mft, 0, sizeof(mft));
    u8 *r0 = mft;
    memcpy(r0, "FILE", 4);
    fmt_le16(r0 + 0x04, 0x30);       /* USA 偏移 */
    fmt_le16(r0 + 0x06, 9);          /* 4096/512 + 1 */
    fmt_le16(r0 + 0x10, 1);          /* 序列号 */
    fmt_le16(r0 + 0x12, 1);          /* 链接数 */
    fmt_le16(r0 + 0x14, 0x38);       /* 属性偏移 */
    fmt_le16(r0 + 0x16, 1);          /* flags: in use */

    u8 *a = r0 + 0x38;
    /* $STANDARD_INFORMATION 常驻（72B） */
    fmt_le32(a, 0x10); fmt_le32(a + 4, 0x60);
    a[8] = 0; a[9] = 0;
    fmt_le16(a + 0x0A, 0x18); fmt_le16(a + 0x0C, 0);
    fmt_le16(a + 0x0E, 0);
    fmt_le32(a + 0x10, 0x48); fmt_le32(a + 0x14, 0x48);
    memset(a + 0x18, 0, 0x48);
    u8 *d = a + 0x60;
    /* $DATA 非常驻：run = 32 簇 @ 簇 4 */
    fmt_le32(d, 0x80); fmt_le32(d + 4, 0x48);    /* 64+3+1=68 */
    d[8] = 1;                                    /* 非常驻 */
    fmt_le16(d + 0x0A, 0x40); fmt_le16(d + 0x0C, 0);
    fmt_le16(d + 0x0E, 1);
    fmt_le32(d + 0x10, 0); fmt_le32(d + 0x14, 0);/* VCN */
    fmt_le32(d + 0x18, 0); fmt_le32(d + 0x1C, 0);
    fmt_le16(d + 0x20, 0x40); fmt_le16(d + 0x22, 0);
    u64 alloc = (u64)32 * 4096;
    fmt_le64(d + 0x28, alloc);
    fmt_le64(d + 0x30, alloc);
    fmt_le64(d + 0x38, alloc);
    d[0x40] = 0x11; d[0x41] = 32; d[0x42] = 4;
    d[0x43] = 0x00;                              /* run 终止 */
    u32 used = 0x38 + 0x60 + 0x48;
    fmt_le32(r0 + 0x18, used);
    fmt_le32(r0 + 0x1C, 0x1FC);                  /* 分配大小（BIOS 对齐项） */
    fmt_le32(r0 + 0x2C, 1);                      /* MFT 记录号 */
    /* fixup */
    for (u32 i = 0; i < 8; i++) {
        u32 pos = (i + 1) * 512 - 2;
        memcpy(r0 + 0x30 + 2 * (i + 1), r0 + pos, 2);
        r0[pos] = 0x01; r0[pos + 1] = 0x00;
    }
    if (blk_write(dev, mft_cluster * cluster, 16, mft) != 0)
        return -3;
    return 0;
}
