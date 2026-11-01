#!/usr/bin/env python3
"""手工构造 FAT32 / ext4 / NTFS 测试镜像。

宿主机没有任何 mkfs 后端（/sbin/mkfs 只是 util-linux 的壳），
所以这里直接按各自的 on-disk 格式写字节。生成的镜像用于在
QEMU 里验证 aptos 内核三个只读文件系统驱动。

用法：python3 tools/mkfs_test.py [输出目录]
"""
import os
import struct
import sys

SECTOR = 512


# =====================================================================
# FAT32
# =====================================================================
def lfn_checksum(short11):
    s = 0
    for b in short11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def dir_entry(name11, attr, cluster, size, is_dir=False):
    e = bytearray(32)
    e[0:11] = name11
    e[11] = attr
    e[12] = 0
    e[13] = 0
    e[14:16] = struct.pack('<H', 0x6000)   # create time
    e[16:18] = struct.pack('<H', 0x5000)   # create date
    e[18:20] = struct.pack('<H', 0x5000)   # access date
    e[20:22] = struct.pack('<H', (cluster >> 16) & 0xFFFF)
    e[22:24] = struct.pack('<H', 0x6000)
    e[24:26] = struct.pack('<H', 0x5000)
    e[26:28] = struct.pack('<H', cluster & 0xFFFF)
    e[28:32] = struct.pack('<I', size)
    return bytes(e)


def lfn_entries(longname, short11):
    """把一个长文件名切成若干 0x0F 目录项（按序号倒序排列）。"""
    u = longname.encode('utf-16-le')
    chunks = [u[i:i + 26] for i in range(0, len(u), 26)]
    csum = lfn_checksum(short11)
    out = []
    for idx, ch in enumerate(chunks):
        e = bytearray(32)
        seq = idx + 1
        if idx == len(chunks) - 1:
            seq |= 0x40          # 最后一项标记
        e[0] = seq
        e[1:11] = ch[0:10].ljust(10, b'\x00')
        e[11] = 0x0F
        e[12] = 0
        e[13] = csum
        e[14:26] = ch[10:22].ljust(12, b'\x00')
        e[26:28] = b'\x00\x00'
        e[28:32] = ch[22:26].ljust(4, b'\x00')
        out.append(bytes(e))
    return b''.join(reversed(out))


def mkfat32(path, size_mb=32):
    bps, spc, rsvd, nfats = 512, 8, 32, 2
    total = size_mb * 2048

    entries = total // spc + 2
    fatsz = (entries * 4 + bps - 1) // bps
    data_start = rsvd + nfats * fatsz
    clusters = (total - data_start) // spc
    assert clusters >= 4085, "太小，会被判成 FAT16"

    img = bytearray(total * bps)
    fat = [0] * (clusters + 2)
    fat[0] = 0x0FFFFFF8
    fat[1] = 0x0FFFFFFF

    next_free = [2]

    def alloc(n=1):
        first = next_free[0]
        c = first
        for i in range(n):
            nxt = c + 1 if i < n - 1 else 0x0FFFFFFF
            fat[c] = nxt
            c += 1
        next_free[0] = first + n
        return first

    def clus_off(c):
        return (data_start + (c - 2) * spc) * bps

    def write_clusters(c, data):
        n = (len(data) + spc * bps - 1) // (spc * bps)
        for i in range(n):
            chunk = data[i * spc * bps:(i + 1) * spc * bps]
            off = clus_off(c) + i * spc * bps
            img[off:off + len(chunk)] = chunk

    root = alloc(1)
    subdir = alloc(1)
    hello_c = alloc(1)
    big_c = alloc(2)
    lfn_c = alloc(1)

    # ---- 引导扇区 ----
    bs = bytearray(bps)
    bs[0:3] = b'\xEB\x58\x90'
    bs[3:11] = b'MSWIN4.1'
    bs[11:13] = struct.pack('<H', bps)
    bs[13] = spc
    bs[14:16] = struct.pack('<H', rsvd)
    bs[16] = nfats
    bs[17:19] = struct.pack('<H', 0)
    bs[19:21] = struct.pack('<H', 0)
    bs[21] = 0xF8
    bs[22:24] = struct.pack('<H', 0)
    bs[24:26] = struct.pack('<H', 63)
    bs[26:28] = struct.pack('<H', 255)
    bs[28:32] = struct.pack('<I', 0)
    bs[32:36] = struct.pack('<I', total)
    bs[36:40] = struct.pack('<I', fatsz)
    bs[40:42] = struct.pack('<H', 0)
    bs[42:44] = struct.pack('<H', 0)
    bs[44:48] = struct.pack('<I', root)
    bs[48:50] = struct.pack('<H', 1)
    bs[50:52] = struct.pack('<H', 6)
    bs[64] = 0x80
    bs[66] = 0x29
    bs[67:71] = struct.pack('<I', 0x12345678)
    bs[71:82] = b'TESTFAT32  '
    bs[82:90] = b'FAT32   '
    bs[510:512] = b'\x55\xAA'
    img[0:bps] = bs

    # ---- FSInfo ----
    fs = bytearray(bps)
    fs[0:4] = struct.pack('<I', 0x41615252)
    fs[0x1E4:0x1E8] = struct.pack('<I', 0x61417272)
    free_cl = clusters - 6
    fs[0x1E8:0x1EC] = struct.pack('<I', free_cl)
    fs[0x1EC:0x1F0] = struct.pack('<I', next_free[0])
    fs[0x1FC:0x200] = struct.pack('<I', 0xAA550000)
    img[1 * bps:2 * bps] = fs

    # ---- FAT 表 x2 ----
    fatbytes = bytearray(fatsz * bps)
    for i, v in enumerate(fat):
        fatbytes[i * 4:i * 4 + 4] = struct.pack('<I', v)
    for f in range(nfats):
        off = (rsvd + f * fatsz) * bps
        img[off:off + len(fatbytes)] = fatbytes

    # ---- 根目录 ----
    rd = bytearray()
    rd += dir_entry(b'TESTFAT32  ', 0x08, 0, 0)              # 卷标
    rd += dir_entry(b'HELLO   TXT', 0x20, hello_c, len(b'Hello from FAT32!\n'))
    rd += lfn_entries('This is a long file name.txt', b'THISIS~1TXT')
    rd += dir_entry(b'THISIS~1TXT', 0x20, lfn_c, len(b'long name file\n'))
    rd += dir_entry(b'SUBDIR     ', 0x10, subdir, 0)
    rd += dir_entry(b'BIG     BIN', 0x20, big_c, 8000)
    write_clusters(root, bytes(rd))

    # ---- 子目录 ----
    sd = bytearray()
    sd += dir_entry(b'.          ', 0x10, subdir, 0)
    sd += dir_entry(b'..         ', 0x10, root, 0)
    sd += dir_entry(b'INNER   TXT', 0x20, alloc(1), len(b'inner file\n'))
    write_clusters(subdir, bytes(sd))

    write_clusters(hello_c, b'Hello from FAT32!\n')
    write_clusters(lfn_c, b'long name file\n')
    big = bytes((i * 7 + 3) & 0xFF for i in range(8000))
    write_clusters(big_c, big)

    open(path, 'wb').write(img)
    return dict(type='fat32', size=len(img), label='TESTFAT32',
                files=['HELLO.TXT', 'This is a long file name.txt',
                       'SUBDIR', 'BIG.BIN'])


# =====================================================================
# ext4 (ext2 布局 + extent 树)
# =====================================================================
EXT4_MAGIC = 0xEF53
S_IFDIR = 0x4000
S_IFREG = 0x8000
EXTENTS_FL = 0x00080000


def mk_extent_root(extents):
    """extents: [(lblock, nblocks, phys), ...] -> 60 字节 inode block 区"""
    buf = bytearray(60)
    struct.pack_into('<HHHHI', buf, 0, 0xF30A, len(extents), 4, 0, 0)
    off = 12
    for lb, ln, ph in extents[:4]:
        struct.pack_into('<IHHI', buf, off, lb, ln, ph >> 32, ph & 0xFFFFFFFF)
        off += 12
    return bytes(buf)


def ext4_dirent(ino, name, ftype, rec_len):
    n = name if isinstance(name, bytes) else name.encode()
    e = bytearray(rec_len)
    struct.pack_into('<IHBB', e, 0, ino, rec_len, len(n), ftype)
    e[8:8 + len(n)] = n
    return bytes(e)


def build_dir_block(entries, block_size):
    """entries: [(ino, name, ftype)]，最后一项 rec_len 填满整块"""
    out = bytearray()
    for i, (ino, name, ftype) in enumerate(entries):
        n = name if isinstance(name, bytes) else name.encode()
        need = 8 + len(n)
        need = (need + 3) & ~3
        if i == len(entries) - 1:
            rec = block_size - len(out)
        else:
            rec = need
        out += ext4_dirent(ino, n, ftype, rec)
    assert len(out) == block_size, (len(out), block_size)
    return bytes(out)


def mk_ext4(path, size_mb=8):
    bs = 1024
    total_blocks = size_mb * 1024
    first_data_block = 1
    blocks_per_group = total_blocks
    inodes_per_group = 2048
    inode_size = 128

    bmp_block = 3
    ino_bmp_block = 4
    inode_table = 5
    inode_table_blocks = (inodes_per_group * inode_size + bs - 1) // bs
    first_meta_free = inode_table + inode_table_blocks

    img = bytearray(total_blocks * bs)
    blk_used = set()
    for b in range(first_data_block, first_meta_free):
        blk_used.add(b)

    ino_used = set([1, 2, 3, 4, 5, 6])
    next_block = [first_meta_free]

    def ab(n=1):
        first = next_block[0]
        for i in range(n):
            blk_used.add(first + i)
        next_block[0] = first + n
        return first

    def wblk(b, data):
        img[b * bs:b * bs + len(data)] = data

    # ---- 数据块分配 ----
    root_blk = ab(1)
    sub_blk = ab(1)
    hello_blk = ab(1)
    big_a = ab(2)
    big_b = ab(1)
    trad_blk = ab(1)
    inner_blk = ab(1)

    # ---- 超级块（块 1，偏移 1024）----
    sb = bytearray(1024)
    free_blocks = total_blocks - len(blk_used)
    struct.pack_into('<IIIIIIIII', sb, 0x00,
                     inodes_per_group, total_blocks, 0, free_blocks, 0,
                     first_data_block, 0, 0, blocks_per_group)
    struct.pack_into('<II', sb, 0x24, blocks_per_group, inodes_per_group)
    struct.pack_into('<IIHHH', sb, 0x2C, 0, 0, 1, 0xFFFF, EXT4_MAGIC)
    struct.pack_into('<H', sb, 0x3A, 1)          # state = clean
    struct.pack_into('<III', sb, 0x4C, 1, 0, 11)      # rev_level, resuid/gid region
    struct.pack_into('<H', sb, 0x58, inode_size)
    sb[0x78:0x78 + 8] = b'TESTEXT4'
    wblk(first_data_block, sb)

    # ---- 块组描述符（块 2）----
    gd = bytearray(bs)
    struct.pack_into('<IIIHHH', gd, 0,
                     bmp_block, ino_bmp_block, inode_table,
                     free_blocks, inodes_per_group - len(ino_used), 1)
    wblk(2, gd)

    # ---- 块位图 ----
    bmap = bytearray(bs)
    for b in blk_used:
        idx = b - first_data_block
        bmap[idx // 8] |= 1 << (idx % 8)
    wblk(bmp_block, bmap)

    # ---- inode 位图 ----
    ibmap = bytearray(bs)
    for i in ino_used:
        ibmap[(i - 1) // 8] |= 1 << ((i - 1) % 8)
    wblk(ino_bmp_block, ibmap)

    # ---- inode 表 ----
    itable = bytearray(inode_table_blocks * bs)

    def put_inode(ino, mode, size, blocks, block_area, links=1, flags=0):
        off = (ino - 1) * inode_size
        struct.pack_into('<HHIIIIHHII', itable, off,
                         mode, 0, size & 0xFFFFFFFF, 0, 0, 0, 0, 0,
                         links, blocks)
        struct.pack_into('<I', itable, off + 0x20, flags)
        itable[off + 0x28:off + 0x28 + len(block_area)] = block_area
        return off

    # inode 1: bad blocks（约定占位）
    put_inode(1, 0, 0, 0, b'')

    root_dir = build_dir_block([
        (2, '.', 2), (2, '..', 2),
        (4, 'hello.txt', 1), (3, 'subdir', 2),
        (5, 'big.bin', 1), (6, 'trad.bin', 1),
    ], bs)
    put_inode(2, S_IFDIR | 0o755, bs, 2, mk_extent_root([(0, 1, root_blk)]),
              links=3, flags=EXTENTS_FL)

    sub_dir = build_dir_block([
        (3, '.', 2), (2, '..', 2), (7, 'inner.txt', 1),
    ], bs)
    put_inode(3, S_IFDIR | 0o755, bs, 2, mk_extent_root([(0, 1, sub_blk)]),
              links=2, flags=EXTENTS_FL)

    hello = b'Hello from ext4!\n'
    wblk(hello_blk, hello)
    put_inode(4, S_IFREG | 0o644, len(hello), 2,
              mk_extent_root([(0, 1, hello_blk)]), flags=EXTENTS_FL)

    big = bytes((i * 13 + 5) & 0xFF for i in range(3072))
    wblk(big_a, big[:2048])
    wblk(big_b, big[2048:])
    put_inode(5, S_IFREG | 0o644, len(big), 6,
              mk_extent_root([(0, 2, big_a), (2, 1, big_b)]), flags=EXTENTS_FL)

    trad = b'traditional indirect-block file\n'
    wblk(trad_blk, trad)
    # 传统映射：直接块放 block[0]（无 EXTENTS 标志）
    put_inode(6, S_IFREG | 0o644, len(trad), 2,
              struct.pack('<I', trad_blk))

    inner = b'inner file in subdir\n'
    wblk(inner_blk, inner)
    put_inode(7, S_IFREG | 0o644, len(inner), 2,
              mk_extent_root([(0, 1, inner_blk)]), flags=EXTENTS_FL)

    for i in range(inode_table_blocks):
        wblk(inode_table + i, itable[i * bs:(i + 1) * bs])

    wblk(root_blk, root_dir)
    wblk(sub_blk, sub_dir)

    open(path, 'wb').write(img)
    return dict(type='ext4', size=len(img), label='TESTEXT4',
                files=['hello.txt', 'subdir/', 'big.bin', 'trad.bin'])


# =====================================================================
# NTFS
# =====================================================================
AT_STANDARD_INFO = 0x10
AT_FILE_NAME = 0x30
AT_VOLUME_NAME = 0x60
AT_VOLUME_INFO = 0x70
AT_DATA = 0x80
AT_INDEX_ROOT = 0x90
AT_INDEX_ALLOC = 0xA0
AT_BITMAP = 0xB0
AT_END = 0xFFFFFFFF

NF_DIR = 0x10000000


def ntfs_fixup_write(rec, usn=0x0001, usa_off=0x30):
    """写入时施加更新序列：把每个扇区尾两字节搬到 USA，再填 USN。"""
    nsec = len(rec) // 512
    usa_count = nsec + 1
    struct.pack_into('<HH', rec, 4, usa_off, usa_count)
    struct.pack_into('<H', rec, usa_off, usn)
    for i in range(nsec):
        pos = (i + 1) * 512 - 2
        rec[usa_off + 2 * (i + 1):usa_off + 2 * (i + 1) + 2] = rec[pos:pos + 2]
        struct.pack_into('<H', rec, pos, usn)
    return rec


def attr_resident(atype, value, attr_id=0):
    hdr = 24
    total = (hdr + len(value) + 7) & ~7
    a = bytearray(total)
    struct.pack_into('<IIBBHHH', a, 0, atype, total, 0, 0, 0, 0, attr_id)
    struct.pack_into('<IHB', a, 16, len(value), hdr, 0)
    a[hdr:hdr + len(value)] = value
    return bytes(a)


def attr_nonresident(atype, runs, real_size, alloc_size, attr_id=0):
    hdr = 64
    rl = encode_runs(runs)
    total = (hdr + len(rl) + 7) & ~7
    a = bytearray(total)
    struct.pack_into('<IIBBHHH', a, 0, atype, total, 1, 0, 0, 0, attr_id)
    # start_vcn, last_vcn
    total_clusters = sum(r[1] for r in runs)
    struct.pack_into('<QQ', a, 16, 0, max(0, total_clusters - 1))
    struct.pack_into('<HH', a, 32, hdr, 0)
    struct.pack_into('<QQQ', a, 40, alloc_size, real_size, real_size)
    a[hdr:hdr + len(rl)] = rl
    return bytes(a)


def encode_runs(runs):
    """runs: [(lcn, count)] -> run list 字节（相对增量、有符号）"""
    out = bytearray()
    prev = 0
    for lcn, cnt in runs:
        lb = max(1, (cnt.bit_length() + 7) // 8)
        delta = lcn - prev
        ob = 0
        if delta != 0:
            n = delta
            ob = 1
            while True:
                v = n >> (8 * ob - 1) if ob < 8 else 0
                # 需要的字节数：能表示 delta 的最少字节
                if -(1 << (8 * ob - 1)) <= delta < (1 << (8 * ob - 1)):
                    break
                ob += 1
                if ob >= 8:
                    break
        out.append((ob << 4) | lb)
        out += cnt.to_bytes(lb, 'little')
        if ob:
            out += (delta & ((1 << (8 * ob)) - 1)).to_bytes(ob, 'little')
        prev = lcn
    out.append(0)
    return bytes(out)


def fname_value(parent, name, real_size, is_dir, alloc_size=None):
    u = name.encode('utf-16-le')
    v = bytearray(0x42 + len(u))
    struct.pack_into('<Q', v, 0, parent)
    struct.pack_into('<QQQ', v, 8, 0, 0, 0)
    struct.pack_into('<QQ', v, 0x20, alloc_size if alloc_size is not None else real_size,
                     real_size)
    struct.pack_into('<I', v, 0x30, NF_DIR if is_dir else 0x20)
    v[0x38] = len(name)
    v[0x39] = 1                      # Win32 命名空间
    v[0x3A:0x3A + len(u)] = u
    return bytes(v)


def index_entry(rec_no, fname_val, length, flags=0):
    e = bytearray(length)
    struct.pack_into('<Q', e, 0, rec_no)
    struct.pack_into('<HHH', e, 8, length, len(fname_val), flags)
    e[0x10:0x10 + len(fname_val)] = fname_val
    return bytes(e)


def build_index_entries(items, total_alloc):
    """items: [(rec_no, name, size, is_dir)] -> 索引项区（含末项哨兵）"""
    out = bytearray()
    for i, (rno, name, size, isdir) in enumerate(items):
        fv = fname_value(5, name, size, isdir)
        need = (0x10 + len(fv) + 7) & ~7
        nxt = 0
        if i + 1 < len(items):
            nxt = (0x10 + len(fname_value(5, items[i + 1][1],
                                          items[i + 1][2], items[i + 1][3])) + 7) & ~7
        else:
            nxt = 0x10
        # 每项长度：至少能容纳自己的名字，且要为下一项留位
        ln = max(need, nxt)
        out += index_entry(rno, fv, ln, 0)
    # 末项哨兵
    out += index_entry(0, b'', 0x10, 0x2)
    return bytes(out)


def mft_record(rec_no, attrs, is_dir=False, seq=1):
    rec = bytearray(1024)
    rec[0:4] = b'FILE'
    struct.pack_into('<HH', rec, 4, 0x30, 3)
    struct.pack_into('<Q', rec, 8, 0)
    struct.pack_into('<HH', rec, 16, seq, 1)
    attrs_off = 0x38
    struct.pack_into('<H', rec, 20, attrs_off)
    struct.pack_into('<H', rec, 22, 1 | (2 if is_dir else 0))
    struct.pack_into('<II', rec, 24, 0, 1024)
    struct.pack_into('<Q', rec, 32, 0)
    struct.pack_into('<H', rec, 40, len(attrs) + 1)

    off = attrs_off
    aid = 0
    for a in attrs:
        rec[off:off + len(a)] = a
        off += len(a)
        aid += 1
    struct.pack_into('<II', rec, off, AT_END, 0)
    off += 8
    struct.pack_into('<I', rec, 24, off)
    return bytes(ntfs_fixup_write(rec))


def mkntfs(path, size_mb=16):
    bps = 512
    spc = 8
    cs = bps * spc
    total_sectors = size_mb * 2048
    total_clusters = total_sectors // spc

    MFT_LCN = 4
    MFT_CLUSTERS = 32
    MFT_REC_SIZE = 1024

    img = bytearray(total_sectors * bps)

    def wclus(c, data):
        off = c * cs
        img[off:off + len(data)] = data

    def woctet(off, data):
        img[off:off + len(data)] = data

    # ---- 数据簇分配 ----
    NEXT = [MFT_LCN + MFT_CLUSTERS]

    def ac(n=1):
        f = NEXT[0]
        NEXT[0] = f + n
        return f

    root_idx_clus = ac(1)      # INDX 记录所在簇
    hello_clus = ac(1)
    big_a = ac(2)
    sub_idx_clus = ac(1)
    inner_clus = ac(1)

    hello = b'Hello from NTFS!\n'
    big = bytes((i * 11 + 7) & 0xFF for i in range(6000))
    inner = b'inner file in ntfs subdir\n'

    wclus(hello_clus, hello)
    wclus(big_a, big[:cs])
    # big 的第二段放到不连续簇，测 run list 的负/正增量
    big_b = ac(1)
    wclus(big_b, big[cs:])
    wclus(inner_clus, inner)

    # ---- 目录索引 ----
    root_items = [(8, 'hello.txt', len(hello), False),
                  (9, 'big.bin', len(big), False),
                  (10, 'subdir', 0, True)]
    sub_items = [(11, 'inner.txt', len(inner), False)]

    # $INDEX_ROOT：容量内放一部分，其余进 $INDEX_ALLOCATION
    root_root_items = root_items[:1]
    root_alloc_items = root_items[1:]
    sub_root_items = sub_items
    sub_alloc_items = []

    def index_root_value(items, large):
        ents = build_index_entries(items, 4096)
        v = bytearray(0x10 + 0x10 + len(ents))
        struct.pack_into('<III', v, 0, AT_FILE_NAME, 1, 4096)
        v[0x0C] = 1
        hdr = 0x10
        struct.pack_into('<IIII', v, hdr, 0x10, len(ents), len(ents),
                         1 if large else 0)
        v[hdr + 0x10:hdr + 0x10 + len(ents)] = ents
        return bytes(v)

    def indx_record(vcn, items):
        rec = bytearray(4096)
        rec[0:4] = b'INDX'
        struct.pack_into('<HH', rec, 4, 0x28, 9)
        struct.pack_into('<Q', rec, 8, 0)
        struct.pack_into('<Q', rec, 0x10, vcn)
        ents = build_index_entries(items, 4096)
        hdr = 0x18
        # USA 数组占用 rec[0x28 .. 0x28+2*9)（usa_off=0x28，9 项），
        # 条目区必须避开它：真实 NTFS 把 entries_off 放在
        # usa_off + 2*usa_count 之后（本例 0x28+18=0x3A → 对齐 0x38 后仍
        # 与 USA 尾部冲突，故取 0x40）。同时条目区起点相对索引头记录。
        ent_off = 0x40 - hdr            # = 0x28，即 rec 偏移 0x40
        struct.pack_into('<IIII', rec, hdr, ent_off, len(ents), len(ents), 1)
        rec[hdr + ent_off:hdr + ent_off + len(ents)] = ents
        return bytes(ntfs_fixup_write(rec, usn=0x0009, usa_off=0x28))

    # ---- 卷标 ----
    volname = 'TESTNTFS'.encode('utf-16-le')

    # ---- MFT 记录 ----
    mft_runs = [(MFT_LCN, MFT_CLUSTERS)]
    mft_data_real = MFT_CLUSTERS * cs
    recs = []
    recs.append(mft_record(0, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, '$MFT', 0, False)),
        attr_nonresident(AT_DATA, mft_runs, mft_data_real, mft_data_real),
    ]))
    recs.append(mft_record(1, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, '$MFTMirr', 0, False)),
        attr_nonresident(AT_DATA, [(2, 4)], 4 * cs, 4 * cs),
    ]))
    recs.append(mft_record(2, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, '$LogFile', 0, False)),
        attr_nonresident(AT_DATA, [(64, 4)], 4 * cs, 4 * cs),
    ]))
    recs.append(mft_record(3, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, '$Volume', 0, False)),
        attr_resident(AT_VOLUME_NAME, volname),
        attr_resident(AT_VOLUME_INFO, bytes(12)),
        attr_nonresident(AT_DATA, [], 0, 0),
    ]))
    recs.append(mft_record(4, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, '$AttrDef', 0, False)),
        attr_nonresident(AT_DATA, [(70, 4)], 4 * cs, 4 * cs),
    ]))

    # 根目录（记录 5）：$INDEX_ROOT + $INDEX_ALLOCATION + $BITMAP
    root_attrs = [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, '.', 0, True)),
        attr_resident(AT_INDEX_ROOT, index_root_value(root_root_items, True)),
        attr_nonresident(AT_INDEX_ALLOC, [(root_idx_clus, 1)], 4096, 4096),
        attr_resident(AT_BITMAP, b'\x01\x00\x00\x00'),
    ]
    recs.append(mft_record(5, root_attrs, is_dir=True))

    recs.append(mft_record(6, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, '$Bitmap', 0, False)),
        attr_nonresident(AT_DATA, [(80, 4)], 4 * cs, 4 * cs),
    ]))
    recs.append(mft_record(7, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, '$Boot', 0, False)),
        attr_nonresident(AT_DATA, [(0, 1)], cs, cs),
    ]))
    # 文件
    recs.append(mft_record(8, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, 'hello.txt', len(hello), False)),
        attr_resident(AT_DATA, hello),
    ]))
    recs.append(mft_record(9, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, 'big.bin', len(big), False)),
        attr_nonresident(AT_DATA, [(big_a, 2), (big_b, 1)], len(big),
                         ((len(big) + cs - 1) // cs) * cs),
    ]))
    # 子目录（记录 10）
    recs.append(mft_record(10, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, 'subdir', 0, True)),
        attr_resident(AT_INDEX_ROOT, index_root_value(sub_root_items, False)),
    ], is_dir=True))
    recs.append(mft_record(11, [
        attr_resident(AT_STANDARD_INFO, bytes(72)),
        attr_resident(AT_FILE_NAME, fname_value(5, 'inner.txt', len(inner), False)),
        attr_resident(AT_DATA, inner),
    ]))

    # 写入 MFT 区
    mft_bytes = bytearray(MFT_CLUSTERS * cs)
    for i, r in enumerate(recs):
        mft_bytes[i * MFT_REC_SIZE:(i + 1) * MFT_REC_SIZE] = r
    wclus(MFT_LCN, bytes(mft_bytes))

    # $MFTMirr：前 4 条记录的副本
    wclus(2, bytes(mft_bytes[:4 * MFT_REC_SIZE]))

    # 索引记录
    wclus(root_idx_clus, indx_record(0, root_alloc_items))

    # ---- 引导扇区 ----
    bs = bytearray(bps)
    bs[0:3] = b'\xEB\x52\x90'
    bs[3:11] = b'NTFS    '
    struct.pack_into('<H', bs, 11, bps)
    bs[13] = spc
    struct.pack_into('<H', bs, 14, 0)
    bs[16] = 0
    struct.pack_into('<H', bs, 17, 0)
    bs[21] = 0xF8
    struct.pack_into('<H', bs, 22, 0)
    struct.pack_into('<H', bs, 24, 63)
    struct.pack_into('<H', bs, 26, 255)
    struct.pack_into('<I', bs, 28, 0)
    struct.pack_into('<I', bs, 32, 0x00800080)
    struct.pack_into('<Q', bs, 40, total_sectors)
    struct.pack_into('<Q', bs, 48, MFT_LCN)
    struct.pack_into('<Q', bs, 56, 2)
    bs[64] = 0xF6               # clusters_per_mft_record = -10 -> 1024
    bs[68] = 1                  # clusters_per_index_record = 1 -> 4096
    struct.pack_into('<Q', bs, 72, 0x1122334455667788)
    bs[510:512] = b'\x55\xAA'
    woctet(0, bytes(bs))

    open(path, 'wb').write(img)
    return dict(type='ntfs', size=len(img), label='TESTNTFS',
                files=['hello.txt', 'big.bin', 'subdir/', 'subdir/inner.txt'])


# =====================================================================
def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else '/tmp'
    os.makedirs(outdir, exist_ok=True)
    results = []
    results.append(mkfat32(os.path.join(outdir, 'test-fat32.img')))
    results.append(mk_ext4(os.path.join(outdir, 'test-ext4.img')))
    results.append(mkntfs(os.path.join(outdir, 'test-ntfs.img')))
    for r in results:
        print("  %-6s %-22s %8d 字节  卷标 %-10s 文件 %s"
              % (r['type'], os.path.basename(
                  {'fat32': 'test-fat32.img', 'ext4': 'test-ext4.img',
                   'ntfs': 'test-ntfs.img'}[r['type']]),
                 r['size'], r['label'], ', '.join(r['files'])))


if __name__ == '__main__':
    main()
