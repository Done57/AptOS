#include "fs.h"
#include "blkdev.h"
#include "bootinfo.h"
#include "klog.h"
#include "ext4.h"
#include "fat32.h"
#include "part.h"
#include "iso9660.h"
#include "minifs.h"
#include "ntfs.h"
#include "util.h"


typedef struct {
    int         used;
    char        point[32];
    int         dev;
    int         type;
    char        label[32];
    int         writable;
    mfs_state_t mfs;
    iso_state_t iso;
    fat32_state_t fat;
    ntfs_state_t  ntfs;
    ext4_state_t  ext;
} fs_mount_t;

static fs_mount_t mounts[FS_MAX_MOUNTS];
static int boot_dev = -1;

static fs_mount_t *root_mount(void)
{
    for (int i = 0; i < FS_MAX_MOUNTS; i++)
        if (mounts[i].used && strcmp(mounts[i].point, "/") == 0)
            return &mounts[i];
    return NULL;
}

static fs_mount_t *route(const char *path, const char **rel)
{
    fs_mount_t *best = NULL;
    int bestlen = -1;

    for (int i = 0; i < FS_MAX_MOUNTS; i++) {
        fs_mount_t *m = &mounts[i];
        int l;
        char c;

        if (!m->used)
            continue;
        l = (int)strlen(m->point);
        if (strncmp(path, m->point, (size_t)l) != 0)
            continue;
        c = path[l];
        if (!(l == 1 || c == 0 || c == '/'))
            continue;
        if (l > bestlen) {
            best = m;
            bestlen = l;
        }
    }
    if (!best)
        return NULL;
    if (bestlen == 1)
        *rel = path;
    else
        *rel = path + bestlen;
    if (**rel == 0)
        *rel = "/";
    return best;
}

static fs_mount_t *free_slot(void)
{
    for (int i = 0; i < FS_MAX_MOUNTS; i++)
        if (!mounts[i].used)
            return &mounts[i];
    return NULL;
}

static fs_mount_t *find_mount(const char *point)
{
    for (int i = 0; i < FS_MAX_MOUNTS; i++)
        if (mounts[i].used && strcmp(mounts[i].point, point) == 0)
            return &mounts[i];
    return NULL;
}

const char *fs_type_name(int type)
{
    switch (type) {
    case FS_TYPE_MINIFS:  return "minifs";
    case FS_TYPE_ISO9660: return "iso9660";
    case FS_TYPE_FAT32:   return "fat32";
    case FS_TYPE_NTFS:    return "ntfs";
    case FS_TYPE_EXT4:    return "ext4";
    default:              return "none";
    }
}

static int detect_and_bind(fs_mount_t *m, int dev, u32 base)
{
    const blk_info_t *bi = blk_info(dev);
    if (!bi)
        return -1;

    m->dev = dev;

    if (bi->sector_size == 512 && mfs_bind(&m->mfs, dev, base) == 0) {
        m->type = FS_TYPE_MINIFS;
        m->writable = 1;
        strncpy(m->label, m->mfs.super.label, sizeof(m->label) - 1);
        return 0;
    }

    if (iso_bind(&m->iso, dev) == 0) {
        m->type = FS_TYPE_ISO9660;
        m->writable = 0;
        strncpy(m->label, m->iso.label[0] ? m->iso.label : "iso9660",
                sizeof(m->label) - 1);
        return 0;
    }

    /* FAT32 / NTFS / ext4 都是只读挂载。三者引导区特征互相排斥，
     * 探测顺序只影响速度不影响正确性。*/
    if (bi->sector_size == 512 && fat32_bind(&m->fat, dev) == 0) {
        m->type = FS_TYPE_FAT32;
        m->writable = 0;
        strncpy(m->label, m->fat.label, sizeof(m->label) - 1);
        return 0;
    }

    if (bi->sector_size == 512 && ntfs_bind(&m->ntfs, dev) == 0) {
        m->type = FS_TYPE_NTFS;
        m->writable = 0;
        strncpy(m->label, m->ntfs.label, sizeof(m->label) - 1);
        return 0;
    }

    if (bi->sector_size == 512 && ext4_bind(&m->ext, dev) == 0) {
        m->type = FS_TYPE_EXT4;
        m->writable = 0;
        strncpy(m->label, m->ext.label, sizeof(m->label) - 1);
        return 0;
    }

    return -1;
}

static int ensure_dir(const char *path)
{
    char parent[256];
    char tmp[256];
    char full[256];
    const char *name;

    if (strcmp(path, "/") == 0)
        return 0;
    if (fs_exists(path))
        return 0;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    fs_parent_path(tmp, parent, sizeof(parent));
    name = fs_basename(tmp);
    if (!name[0])
        return -1;

    snprintf(full, sizeof(full), "%s%s%s", parent,
             strcmp(parent, "/") == 0 ? "" : "/", name);
    return fs_mkdir(full);
}

static int device_is_mounted(int dev)
{
    for (int i = 0; i < FS_MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].dev == dev)
            return 1;
    return 0;
}

int fs_mount(const char *devname, const char *point)
{
    int dev = blk_find(devname);
    fs_mount_t *m;
    u32 base = 0;

    if (dev < 0)
        return -2;
    if (!point || point[0] != '/')
        return -3;
    if (find_mount(point))
        return -4;
    if (device_is_mounted(dev))
        return -8;
    if (strcmp(point, "/") != 0 && !fs_mounted())
        return -1;

    m = free_slot();
    if (!m)
        return -5;
    memset(m, 0, sizeof(*m));

    if (dev == boot_dev && blk_info(dev)->type == BLK_HDD)
        base = BOOTINFO->fs_lba;

    if (detect_and_bind(m, dev, base) != 0)
        return -6;

    strncpy(m->point, point, sizeof(m->point) - 1);
    m->used = 1;

    if (strcmp(point, "/") != 0)
        (void)ensure_dir(point);

    kprintf("[vfs] mounted %s (%s) on %s%s\n", blk_info(dev)->name,
            fs_type_name(m->type), point, m->writable ? "" : " [read-only]");
    return 0;
}

int fs_umount(const char *point)
{
    fs_mount_t *m = find_mount(point);
    if (!m)
        return -1;
    if (strcmp(point, "/") == 0)
        return -2;
    kprintf("[vfs] unmounted %s\n", point);
    m->used = 0;
    return 0;
}

/* ---- 按类型格式化 ----
 * minifs/fat32 完整实现；ntfs/ext4 写"空卷骨架"（引导扇区/超级块 +
 * 空 $MFT / 空组描述符），足以被探测识别并挂成只读空卷 ——
 * 对安装器来说"格式化成 NTFS"的意义就是盘上有可识别的空文件系统。
 * 成功后自动重新挂载到 /mnt/<设备名>。*/
int fs_format_type(const char *devname, int type, const char *label)
{
    int dev = blk_find(devname);
    const blk_info_t *bi;

    if (dev < 0)
        return -2;
    bi = blk_info(dev);
    if (!bi->writable || bi->sector_size != 512)
        return -3;

    /* 已挂载就先卸载（根分区不格式化 —— 保护自己） */
    for (int i = 0; i < FS_MAX_MOUNTS; i++) {
        if (mounts[i].used && mounts[i].dev == dev) {
            if (strcmp(mounts[i].point, "/") == 0)
                return -7;
            kprintf("[vfs] unmounted %s for format\n", mounts[i].point);
            mounts[i].used = 0;
        }
    }
    if (dev == boot_dev)
        return -8;                       /* 引导盘不格式化 */

    int r;
    switch (type) {
    case FS_TYPE_MINIFS:
        r = mfs_format(dev, 0, label ? label : "APTOS", bi->sectors);
        break;
    case FS_TYPE_FAT32:
        r = fat32_format(dev, label ? label : "NO NAME", 8);
        break;
    case FS_TYPE_NTFS:
        r = ntfs_format_empty(dev, label ? label : "NO NAME");
        break;
    case FS_TYPE_EXT4:
        r = ext4_format_empty(dev, label ? label : "NO NAME");
        break;
    default:
        return -9;
    }
    if (r != 0)
        return r;

    kprintf("[vfs] formatted %s as %s (label '%s')\n", devname,
            fs_type_name(type), label ? label : "");
    return 0;
}

/* ---- 整盘单分区（给安装器用）----
 * ptype: 0x0B FAT32 / 0x07 NTFS / 0x83 Linux。返回新分区设备名。*/
int fs_part_create_single(const char *devname, u8 ptype,
                          char *out_name, int out_max)
{
    int dev = blk_find(devname);
    if (dev < 0)
        return -2;
    if (dev == boot_dev)
        return -8;
    return part_create_single(dev, ptype, out_name, out_max);
}

int fs_format(const char *devname, const char *label)
{
    int dev = blk_find(devname);
    const blk_info_t *bi;
    u32 base = 0;

    if (dev < 0)
        return -2;
    bi = blk_info(dev);
    if (!bi->writable || bi->sector_size != 512)
        return -3;
    if (device_is_mounted(dev))
        return -4;
    if (dev == boot_dev)
        base = BOOTINFO->fs_lba;

    kprintf("[vfs] formatting %s at sector %d (%d blocks)\n", bi->name, base,
            bi->sectors);
    if (mfs_format(dev, base, label ? label : "aptos", bi->sectors) != 0)
        return -5;
    return 0;
}

void fs_type_name_of(int type, char *out, int out_max)
{
    const char *n = fs_type_name(type);
    if (!n)
        n = "?";
    snprintf(out, (u32)out_max, "%s", n);
}

int fs_mounts(fs_mountinfo_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < FS_MAX_MOUNTS && n < max; i++) {
        fs_mount_t *m = &mounts[i];
        const blk_info_t *bi;
        if (!m->used)
            continue;
        bi = blk_info(m->dev);
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].point, m->point, sizeof(out[n].point) - 1);
        strncpy(out[n].dev, bi->name, sizeof(out[n].dev) - 1);
        out[n].type = m->type;
        strncpy(out[n].label, m->label, sizeof(out[n].label) - 1);
        out[n].writable = m->writable;
        if (m->type == FS_TYPE_MINIFS) {
            out[n].total_kb = m->mfs.super.total_blocks / 2;
            out[n].free_kb = m->mfs.super.free_blocks / 2;
        } else if (m->type == FS_TYPE_ISO9660) {
            out[n].total_kb = m->iso.root_size ? bi->sectors * 2 : 0;
            out[n].free_kb = 0;
        } else if (m->type == FS_TYPE_FAT32) {
            out[n].total_kb = fat32_total_kb(&m->fat);
            out[n].free_kb  = fat32_free_kb(&m->fat);
        } else if (m->type == FS_TYPE_NTFS) {
            out[n].total_kb = ntfs_total_kb(&m->ntfs);
            out[n].free_kb  = 0;
        } else if (m->type == FS_TYPE_EXT4) {
            out[n].total_kb = ext4_total_kb(&m->ext);
            out[n].free_kb  = ext4_free_kb(&m->ext);
        } else {
            out[n].total_kb = bi->sectors * bi->sector_size / 1024;
            out[n].free_kb = 0;
        }
        n++;
    }
    return n;
}

int fs_init(void)
{
    memset(mounts, 0, sizeof(mounts));
    boot_dev = -1;

    if (blk_count() == 0) {
        kprintf("[vfs] no block devices\n");
        return -1;
    }

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < blk_count(); i++) {
            const blk_info_t *bi = blk_info(i);
            fs_mount_t *m;
            u32 base;

            if (bi->type != BLK_HDD)
                continue;
            m = free_slot();
            if (!m)
                break;
            memset(m, 0, sizeof(*m));
            base = (pass == 0 && i == 0) ? BOOTINFO->fs_lba : 0;
            if (detect_and_bind(m, i, base) == 0) {
                strcpy(m->point, "/");
                m->used = 1;
                boot_dev = i;
                kprintf("[vfs] %s mounted on / (%s, label '%s')\n", bi->name,
                        fs_type_name(m->type), m->label);
                return 0;
            }
            memset(m, 0, sizeof(*m));
        }
    }

    kprintf("[vfs] no filesystem found on any disk\n");
    return -1;
}

/* 把 fs_init 之后剩下的、能被识别的磁盘挂到 /mnt/<设备名>。
 * 开机自动挂载只读卷，用户不需要手敲 mount 就能看到外来磁盘。*/
int fs_automount_all(void)
{
    int n = 0;

    for (int i = 0; i < blk_count(); i++) {
        const blk_info_t *bi = blk_info(i);
        fs_mount_t *m;

        if (!bi || bi->type != BLK_HDD)
            continue;
        if (device_is_mounted(i))
            continue;

        m = free_slot();
        if (!m)
            break;

        memset(m, 0, sizeof(*m));
        if (detect_and_bind(m, i, 0) != 0) {
            kprintf("[vfs] %s: no recognizable filesystem\n", bi->name);
            memset(m, 0, sizeof(*m));
            continue;
        }

        snprintf(m->point, sizeof(m->point), "/mnt/%s", bi->name);
        m->used = 1;
        kprintf("[vfs] %s mounted on %s (%s, label '%s')%s\n",
                bi->name, m->point, fs_type_name(m->type), m->label,
                m->writable ? "" : " [ro]");
        n++;
    }
    return n;
}

#define FS_INSTALL_MAX_BLOCKS (256u * 1024u * 2u)

int fs_install(int dev, u32 base, const char *label)
{
    const blk_info_t *bi = blk_info(dev);
    fs_mount_t *m;
    u32 blocks;
    int capped = 0;

    if (!bi || bi->sector_size != 512)
        return -1;
    if (base >= bi->sectors)
        return -2;
    blocks = bi->sectors - base;
    if (blocks > FS_INSTALL_MAX_BLOCKS) {
        blocks = FS_INSTALL_MAX_BLOCKS;
        capped = 1;
    }
    if (blocks < 512)
        return -3;

    if (mfs_format(dev, base, label, blocks) != 0)
        return -3;

    m = free_slot();
    if (!m)
        return -4;
    memset(m, 0, sizeof(*m));
    if (mfs_bind(&m->mfs, dev, base) != 0) {
        memset(m, 0, sizeof(*m));
        return -5;
    }
    m->dev = dev;
    m->type = FS_TYPE_MINIFS;
    m->writable = 1;
    strncpy(m->label, m->mfs.super.label, sizeof(m->label) - 1);
    strcpy(m->point, "/TARGET");
    m->used = 1;
    boot_dev = dev;
    kprintf("[vfs] %s formatted at LBA %d, mounted on /TARGET (%d blocks = %d MiB%s)\n",
            bi->name, base, blocks, blocks / 2048,
            capped ? ", capped" : "");
    return 0;
}

int fs_install_root(const char *devname)
{
    int dev = blk_find(devname);
    if (dev < 0)
        return -1;
    return fs_install(dev, 0, "aptos");
}

int fs_mounted(void)
{
    return root_mount() != NULL;
}

int fs_resolve(const char *path, fs_dirent_t *out)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    switch (m->type) {
    case FS_TYPE_ISO9660: return iso_resolve(&m->iso, rel, out);
    case FS_TYPE_FAT32:   return fat32_resolve(&m->fat, rel, out);
    case FS_TYPE_NTFS:    return ntfs_resolve(&m->ntfs, rel, out);
    case FS_TYPE_EXT4:    return ext4_resolve(&m->ext, rel, out);
    case FS_TYPE_MINIFS:  break;
    default:              return -1;
    }
    mfs_use(&m->mfs);
    return mfs_resolve(rel, out);
}

int fs_exists(const char *path)
{
    fs_dirent_t e;
    return fs_resolve(path, &e) == 0;
}

int fs_is_dir(const char *path)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);
    fs_dirent_t e;

    if (!m)
        return 0;
    if (strcmp(rel, "/") == 0)
        return 1;
    if (fs_resolve(path, &e) != 0)
        return 0;
    return (e.flags & FS_FLAG_DIR) != 0;
}

int fs_list(const char *path, int index, fs_dirent_t *out)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    switch (m->type) {
    case FS_TYPE_ISO9660: return iso_list(&m->iso, rel, index, out);
    case FS_TYPE_FAT32:   return fat32_list(&m->fat, rel, index, out);
    case FS_TYPE_NTFS:    return ntfs_list(&m->ntfs, rel, index, out);
    case FS_TYPE_EXT4:    return ext4_list(&m->ext, rel, index, out);
    case FS_TYPE_MINIFS:  break;
    default:              return -1;
    }
    mfs_use(&m->mfs);
    return mfs_list(rel, index, out);
}

int fs_count(const char *path)
{
    int n = 0;
    while (fs_list(path, n, NULL) == 0)
        n++;
    return n;
}

int fs_read(const char *path, void *buf, u32 max)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    switch (m->type) {
    case FS_TYPE_ISO9660: return iso_read(&m->iso, rel, buf, max);
    case FS_TYPE_FAT32:   return fat32_read(&m->fat, rel, buf, max);
    case FS_TYPE_NTFS:    return ntfs_read(&m->ntfs, rel, buf, max);
    case FS_TYPE_EXT4:    return ext4_read(&m->ext, rel, buf, max);
    case FS_TYPE_MINIFS:  break;
    default:              return -1;
    }
    mfs_use(&m->mfs);
    return mfs_read(rel, buf, max);
}

int fs_write(const char *path, const void *buf, u32 size)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    if (!m->writable || m->type != FS_TYPE_MINIFS)
        return -2;
    mfs_use(&m->mfs);
    return mfs_write(rel, buf, size);
}

int fs_mkdir(const char *path)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    if (!m->writable || m->type != FS_TYPE_MINIFS)
        return -2;
    mfs_use(&m->mfs);
    return mfs_mkdir(rel);
}

int fs_delete(const char *path)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m)
        return -1;
    if (!m->writable || m->type != FS_TYPE_MINIFS)
        return -2;
    mfs_use(&m->mfs);
    return mfs_delete(rel);
}

int fs_rename(const char *from, const char *to)
{
    const char *rel_a, *rel_b;
    fs_mount_t *ma = route(from, &rel_a);
    fs_mount_t *mb = route(to, &rel_b);

    if (!ma || !mb || ma != mb)
        return -1;
    if (!ma->writable || ma->type != FS_TYPE_MINIFS)
        return -2;
    mfs_use(&ma->mfs);
    return mfs_rename(rel_a, rel_b);
}

void fs_stat(fs_stat_t *out)
{
    fs_mount_t *m = root_mount();

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!m || m->type != FS_TYPE_MINIFS)
        return;
    mfs_use(&m->mfs);
    mfs_stat(out);
}

int fs_count_kind(const char *path, u32 *files, u32 *dirs)
{
    const char *rel;
    fs_mount_t *m = route(path, &rel);

    if (!m || m->type != FS_TYPE_MINIFS)
        return -1;
    mfs_use(&m->mfs);
    return mfs_count_kind(rel, files, dirs);
}

u32 fs_total_kb(void)
{
    fs_mount_t *m = root_mount();
    return (m && m->type == FS_TYPE_MINIFS) ? m->mfs.super.total_blocks / 2 : 0;
}

u32 fs_free_kb(void)
{
    fs_mount_t *m = root_mount();
    return (m && m->type == FS_TYPE_MINIFS) ? m->mfs.super.free_blocks / 2 : 0;
}

const char *fs_volume_label(void)
{
    fs_mount_t *m = root_mount();
    return m ? m->label : "";
}

void fs_path(char *out, u32 outsz, const char *cwd, const char *arg)
{
    char work[256];
    int wl = 0;
    const char *p = arg;

    if (!arg || !arg[0])
        arg = ".";
    if (arg[0] == '/') {
        work[0] = 0;
        wl = 0;
    } else {
        strncpy(work, (cwd && cwd[0]) ? cwd : "/", sizeof(work) - 1);
        work[sizeof(work) - 1] = 0;
        wl = (int)strlen(work);
        if (wl == 1 && work[0] == '/')
            wl = 0;
    }

    while (*p) {
        char comp[FS_MAX_NAME + 1];
        int n = 0;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        while (*p && *p != '/' && n < FS_MAX_NAME)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p && *p != '/')
            p++;

        if (strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            while (wl > 0 && work[wl - 1] != '/')
                wl--;
            if (wl > 0)
                wl--;
            work[wl] = 0;
            continue;
        }
        if (wl + 1 + n >= (int)sizeof(work) - 1)
            break;
        work[wl++] = '/';
        for (int i = 0; i < n; i++)
            work[wl++] = comp[i];
        work[wl] = 0;
    }

    if (wl == 0) {
        work[0] = '/';
        work[1] = 0;
    }
    strncpy(out, work, outsz - 1);
    out[outsz - 1] = 0;
}

const char *fs_basename(const char *path)
{
    const char *slash;
    if (!path)
        return "";
    slash = strrchr(path, '/');
    if (!slash)
        return path;
    if (!slash[1])
        return slash;
    return slash + 1;
}

void fs_parent_path(const char *path, char *out, u32 outsz)
{
    char tmp[256];
    char *slash;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) {
        strncpy(out, "/", outsz - 1);
        out[outsz - 1] = 0;
        return;
    }
    *slash = 0;
    strncpy(out, tmp, outsz - 1);
    out[outsz - 1] = 0;
}
