#include "aptos.h"
#include "i18n.h"

int main(void)
{
    char pkg[160];
    char name[40];
    char dest[200];
    int h, n, i, ok = 0, bytes = 0;

    printf("\n");
    printf("%s\n", T("=== aptos package installer ===", "=== aptos 软件包安装器 ==="));

    if (sys_args(0, pkg, sizeof(pkg)) != 0) {
        printf("%s\n", T("no package given", "没有指定软件包"));
        return 1;
    }
    printf(T("package : %s\n", "软件包 : %s\n"), pkg);

    h = sys_pkg_open(pkg);
    if (h < 0) {
        printf(T("cannot open package (error %d)\n", "无法打开软件包 (错误 %d)\n"), h);
        return 2;
    }
    n = sys_pkg_count(h);
    printf(T("entries : %d\n\n", "条目数 : %d\n\n"), n);

    if (!sys_exists("/APPS")) {
        printf("%s\n", T("creating /APPS ...", "正在创建 /APPS ..."));
        if (sys_mkdir("/APPS") != 0) {
            printf("%s\n", T("cannot create /APPS", "无法创建 /APPS"));
            return 3;
        }
    }

    for (i = 0; i < n; i++) {
        int size = sys_pkg_info(h, i, name);
        int r;

        if (size < 0) {
            printf(T("  entry %d: unreadable\n", "  第 %d 个条目无法读取\n"), i);
            continue;
        }
        snprintf(dest, sizeof(dest), "/APPS/%s.APP", name);
        r = sys_pkg_extract(h, i, dest);
        if (r < 0) {
            printf(T("  %-20s FAILED (%d)\n", "  %-20s 失败 (%d)\n"), name, r);
        } else {
            printf(T("  %-20s %6d bytes  -> %s\n", "  %-20s %6d 字节  -> %s\n"),
                   name, r, dest);
            ok++;
            bytes += r;
        }
    }

    printf(T("\n%d of %d files installed (%d bytes).\n",
             "\n已安装 %d / %d 个文件 (%d 字节)。\n"), ok, n, bytes);
    printf("%s\n", T("they are now on the desktop.", "它们现在就在桌面上。"));
    return 0;
}
