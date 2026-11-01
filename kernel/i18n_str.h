/* aptos - UI string table (English / Chinese).
 *
 * Every line is  I18N(ID, english, chinese).  kernel/i18n.h turns the list
 * into an enum and kernel/i18n.c into the lookup table, so the two can never
 * drift apart.  Add new user-visible strings here, then run
 *   tools/getunifont.sh
 * to make sure the CJK font contains every glyph used by the new text.
 */
/* NOTE: deliberately no include guard - this file is expanded as an X-macro
 * list both by kernel/i18n.h (enum) and by kernel/i18n.c (string table). */

/* ---- window titles and the desktop ---------------------------------- */
I18N(S_APP_TERMINAL,  "Terminal",   "终端")
I18N(S_APP_FILES,     "This PC",    "此电脑")
I18N(S_APP_EDITOR,    "Editor",     "编辑器")
I18N(S_APP_PAINT,     "Paint",      "画板")
I18N(S_APP_ABOUT,     "About",      "关于")
I18N(S_APP_SNAKE,     "Snake",      "贪吃蛇")
I18N(S_APP_MINES,     "Minesweeper","扫雷")
I18N(S_APP_2048,      "2048",       "2048")
I18N(S_APP_SETUP,     "aptos 2.0 Setup", "aptos 2.0 安装程序")

I18N(S_DESK_HINT, "32-bit toy operating system - double-click a desktop icon",
                  "32 位玩具操作系统 - 双击桌面图标打开应用")
I18N(S_START,     "aptos", "aptos")
I18N(S_MENU_LANG, "Language (F1)", "语言 (F1)")
I18N(S_LANG_EN,   "English",       "英文")
I18N(S_LANG_ZH,   "Chinese",       "中文")


/* ---- installer (setup) ---------------------------------------------- */
I18N(S_SETUP_TITLE,    "aptos 2.0 Setup", "aptos 2.0 安装程序")
I18N(S_SETUP_STEP,     "Step %d of 3",    "第 %d 步（共 3 步）")
I18N(S_SETUP_PICKDISK, "Select a target disk:", "选择目标磁盘：")
I18N(S_SETUP_DISKINFO, "%s  %u MB",        "%s  %u MB")
I18N(S_SETUP_HASPART,  "(has partitions)",  "（已有分区）")
I18N(S_SETUP_EMPTY,    "(blank)",           "（空白盘）")
I18N(S_SETUP_PICKFS,   "Choose filesystem for the new partition:",
                       "为新分区选择文件系统：")
I18N(S_SETUP_FS_MINIFS,"aptos MINIFS (native, read-write)",
                            "aptos MINIFS（原生，可读写）")
I18N(S_SETUP_FS_FAT32, "FAT32 (Windows compatible)", "FAT32（Windows 兼容）")
I18N(S_SETUP_FS_NTFS,  "NTFS  (Windows, read-only support)",
                            "NTFS（Windows，只读支持）")
I18N(S_SETUP_FS_EXT4,  "ext4  (Linux, read-only support)",
                            "ext4（Linux，只读支持）")
I18N(S_SETUP_WARN,     "ALL DATA on %s WILL BE DESTROYED!", "磁盘 %s 上的所有数据将被清除！")
I18N(S_SETUP_PARTITION,"Partition & format", "分区并格式化")
I18N(S_SETUP_BACK,     "Back", "上一步")
I18N(S_SETUP_WORKING,  "Working...", "正在处理…")
I18N(S_SETUP_DONE_OK,  "Done! %s formatted as %s and mounted.", "完成！%s 已格式化为 %s 并挂载。")
I18N(S_SETUP_DONE_FAIL,"Failed (code %d).", "失败（代码 %d）。")
I18N(S_SETUP_COPYING,  "Installing system files to %s ...", "正在向 %s 安装系统文件…")
I18N(S_SETUP_INSTALLED,"System installed. Reboot to start from the new disk.",
                       "系统已安装。重启后可从新硬盘引导。")
I18N(S_SETUP_INSTFAIL, "System file install failed (%d).", "系统文件安装失败（%d）。")
I18N(S_SETUP_EXIT,     "Close", "关闭")
I18N(S_SETUP_MOUNTED,  "mounted at %s", "挂载于 %s")
I18N(S_MYPC_DRIVES,    "Drives (%d)",   "磁盘（%d 个）")
I18N(S_MYPC_FSNONE,    "no filesystem", "无法识别的文件系统")
I18N(S_MYPC_RO,        "read-only",     "只读")
I18N(S_MYPC_RW,        "read-write",    "可读写")
/* ---- shared widgets -------------------------------------------------- */
I18N(S_UI_UP,      "Up",       "上一级")
I18N(S_UI_REFRESH, "Refresh",  "刷新")
I18N(S_UI_SAVE,    "Save",     "保存")
I18N(S_UI_CLEAR,   "Clear",    "清空")

/* ---- About ----------------------------------------------------------- */
I18N(S_ABOUT_VERSION, "version 2.0  -  a small 32-bit x86 operating system",
                      "版本 2.0  -  一个小型 32 位 x86 操作系统")
I18N(S_ABOUT_CPU,     "cpu      : i386 protected mode, 4 KiB paging, ring 3 ready",
                      "处理器   : i386 保护模式, 4 KiB 分页, ring 3 就绪")
I18N(S_ABOUT_DISPLAY, "display  : %dx%d, %d bpp linear framebuffer",
                      "显示器   : %dx%d, %d 位色线性帧缓冲")
I18N(S_ABOUT_MEMORY,  "memory   : %d MiB usable, heap %d KiB used of %d MiB",
                      "内存     : 可用 %d MiB, 堆已用 %d KiB / %d MiB")
I18N(S_ABOUT_DISK,    "disk     : %s", "磁盘     : %s")
I18N(S_ABOUT_VOLUME,  "volume   : MINIFS '%s', %d KiB free",
                      "卷       : MINIFS '%s', 剩余 %d KiB")
I18N(S_ABOUT_UPTIME,  "uptime   : %d.%03d s", "运行时间 : %d.%03d 秒")
I18N(S_ABOUT_BOOT,    "bootloader -> protected mode -> C kernel -> this window",
                      "引导程序 -> 保护模式 -> C 内核 -> 这个窗口")
I18N(S_ABOUT_TIP,     "Try the Terminal: ls, cat README.TXT, dmesg, paint, files",
                      "试试终端: ls, cat README.TXT, dmesg, paint, files")

/* ---- Files ----------------------------------------------------------- */
I18N(S_FILES_INFO,  "%d files, %d dirs, %d KiB free",
                    "%d 个文件, %d 个目录, 剩余 %d KiB")
I18N(S_FILES_PARENT,"[DIR]  ..", "[目录] ..")
I18N(S_FILES_DIRTAG, "[DIR]", "[目录]")
I18N(S_FILES_UP,    "up", "上级")
I18N(S_FILES_EMPTY, "(empty directory)", "(空目录)")
I18N(S_FILES_HINT,  "folder = open, .lst = install, other file = edit",
                    "文件夹 = 打开, .lst = 安装, 其它文件 = 编辑")

/* ---- Editor ---------------------------------------------------------- */
I18N(S_ED_NONAME,  "(no name)", "(未命名)")
I18N(S_ED_UNSAVED, "* unsaved   Ctrl+S saves", "* 未保存   Ctrl+S 保存")
I18N(S_ED_SAVED,   "saved", "已保存")
I18N(S_ED_STATUS,  "Ln %d, Col %d   %d bytes", "第 %d 行, 第 %d 列   %d 字节")

/* ---- games ----------------------------------------------------------- */
I18N(S_SNAKE_INFO, "SCORE %d    LENGTH %d    SPEED %dms",
                   "得分 %d    长度 %d    速度 %d 毫秒")
I18N(S_SNAKE_OVER, "GAME OVER - press R", "游戏结束 - 按 R 重来")
I18N(S_MINES_INFO, "MINES %d   FLAGS %d   LEFT %d",
                   "地雷 %d   旗帜 %d   剩余 %d")
I18N(S_MINES_WIN,  "YOU WIN - press R", "你赢了 - 按 R 重来")
I18N(S_MINES_BOOM, "BOOM - press R",    "踩雷了 - 按 R 重来")
I18N(S_MINES_HINT, "left click opens, right click flags, R restarts",
                   "左键翻开, 右键插旗, R 重新开始")
I18N(S_2048_INFO,  "SCORE %d    BEST %d", "得分 %d    最高 %d")
I18N(S_2048_NOMOVE,"NO MOVES - press R",  "无路可走 - 按 R 重来")
I18N(S_2048_HINT,  "arrows to play",      "方向键操作")

/* ---- terminal: chrome ------------------------------------------------ */
I18N(S_TERM_WELCOME, "aptos terminal - type 'help' for the command list.",
                     "aptos 终端 - 输入 help 查看命令列表。")
I18N(S_TERM_MOUNTED, "MINIFS02 is mounted at /, try: ls, cd /DOCS, pwd",
                     "MINIFS02 已挂载到 /, 试试: ls, cd /DOCS, pwd")
I18N(S_TERM_NOTFOUND, "%s: command not found (try 'help')",
                      "%s: 找不到该命令 (试试 help)")
I18N(S_TERM_OPENED,   "opened %s", "已打开 %s")
I18N(S_TERM_REBOOT,   "rebooting...", "正在重启...")
I18N(S_TERM_POWEROFF, "powering off via ACPI S5...", "正在通过 ACPI S5 关机...")
I18N(S_TERM_APPS,     "applications:", "应用程序:")
I18N(S_TERM_VER,      "aptos 2.0 - 32-bit x86, C kernel, VBE framebuffer",
                      "aptos 2.0 - 32 位 x86, C 内核, VBE 帧缓冲")
I18N(S_TERM_DF,       "volume %s: %d KiB total, %d KiB free, %d files, %d dirs",
                      "卷 %s: 共 %d KiB, 剩余 %d KiB, %d 个文件, %d 个目录")
I18N(S_TERM_MEM,      "RAM: %d MiB total, heap used %d KiB of %d KiB",
                      "内存: 共 %d MiB, 堆已用 %d KiB / %d KiB")
I18N(S_TERM_UPTIME,   "up %d.%03d s (%d ticks at %d Hz)",
                      "已运行 %d.%03d 秒 (%d 个时钟滴答, %d Hz)")
I18N(S_USAGE_CAT,     "usage: cat <file>", "用法: cat <文件>")
I18N(S_USAGE_WRITE,   "usage: write <file> <text>", "用法: write <文件> <内容>")
I18N(S_USAGE_MKDIR,   "usage: mkdir <name>", "用法: mkdir <名称>")
I18N(S_USAGE_RM,      "usage: rm <name>", "用法: rm <名称>")
I18N(S_USAGE_CP,      "usage: cp <source> <dest>", "用法: cp <源文件> <目标>")
I18N(S_USAGE_MV,      "usage: mv <source> <dest>", "用法: mv <源文件> <目标>")
I18N(S_USAGE_INSTALL, "usage: install <package.lst>", "用法: install <安装包.lst>")
I18N(S_USAGE_PKGINFO, "usage: pkginfo <package.lst>", "用法: pkginfo <安装包.lst>")
I18N(S_USAGE_RUN,     "usage: run <executable>", "用法: run <可执行文件>")
I18N(S_USAGE_MOUNT,   "usage: mount <device> <mountpoint>", "用法: mount <设备> <挂载点>")
I18N(S_USAGE_UMOUNT,  "usage: umount <mountpoint>", "用法: umount <挂载点>")
I18N(S_USAGE_MKFS,    "usage: mkfs <device>", "用法: mkfs <设备>")

/* ---- terminal: language command -------------------------------------- */
I18N(S_LANG_USAGE, "usage: lang [zh|en]   (now: %s)",
                   "用法: lang [zh|en]   (当前: %s)")
I18N(S_LANG_SET,   "language: %s", "语言: %s")

/* ---- terminal: ls / cd / cat / write --------------------------------- */
I18N(S_LS_NOTDIR,  "ls: %s: not a directory", "ls: %s: 不是目录")
I18N(S_LS_DIR,     "directory %s", "目录 %s")
I18N(S_LS_NAME,    "NAME", "名称")
I18N(S_LS_SIZE,    "SIZE", "大小")
I18N(S_LS_DATE,    "DATE", "日期")
I18N(S_LS_SUMMARY, "%d entries, %d KiB free", "%d 个条目, 剩余 %d KiB")
I18N(S_LS_DIRTAG,  "<DIR>", "<目录>")
I18N(S_CD_NODIR,   "cd: %s: no such directory", "cd: %s: 目录不存在")
I18N(S_CAT_ISDIR,  "cat: %s: is a directory", "cat: %s: 这是一个目录")
I18N(S_CAT_NOFILE, "cat: %s: no such file", "cat: %s: 文件不存在")
I18N(S_WRITE_FAIL, "write: could not save %s", "write: 无法保存 %s")
I18N(S_WRITE_OK,   "wrote %d bytes to %s", "已写入 %d 字节到 %s")
I18N(S_MKDIR_OK,   "created %s", "已创建 %s")
I18N(S_MKDIR_FAIL, "mkdir: cannot create %s", "mkdir: 无法创建 %s")
I18N(S_RM_OK,      "removed %s", "已删除 %s")
I18N(S_RM_NOTEMPTY,"rm: %s: directory not empty", "rm: %s: 目录非空")
I18N(S_RM_NOFILE,  "rm: %s: no such file or directory", "rm: %s: 文件或目录不存在")
I18N(S_CP_NOFILE,  "cp: %s: no such file", "cp: %s: 文件不存在")
I18N(S_CP_FAIL,    "cp: cannot write %s", "cp: 无法写入 %s")
I18N(S_CP_OK,      "copied %s -> %s (%d bytes)", "已复制 %s -> %s (%d 字节)")
I18N(S_MV_OK,      "moved %s -> %s", "已移动 %s -> %s")
I18N(S_MV_EXISTS,  "mv: %s: destination exists", "mv: %s: 目标已存在")
I18N(S_MV_NOFILE,  "mv: %s: no such file or directory", "mv: %s: 文件或目录不存在")

/* ---- terminal: packages and programs --------------------------------- */
I18N(S_INSTALL_NOPKG, "install: %s: no such package", "install: %s: 安装包不存在")
I18N(S_INSTALL_OPEN,  "opening package %s", "正在打开安装包 %s")
I18N(S_INSTALL_DONE,  "installation finished.", "安装完成。")
I18N(S_INSTALL_FAIL,  "installation failed (%d)", "安装失败 (%d)")
I18N(S_PKGI_NOFILE,   "pkginfo: %s: no such file", "pkginfo: %s: 文件不存在")
I18N(S_PKGI_INVALID,  "pkginfo: %s: not a valid .lst package",
                      "pkginfo: %s: 不是有效的 .lst 安装包")
I18N(S_PKGI_NAME,     "package    : %s", "包名       : %s")
I18N(S_PKGI_VENDOR,   "vendor     : %s", "厂商       : %s")
I18N(S_PKGI_DESC,     "description: %s", "描述       : %s")
I18N(S_PKGI_ENTRIES,  "entries    : %d (installer is #%d)",
                      "条目       : %d (安装程序是第 %d 个)")
I18N(S_PKGI_INSTALLER,"  <- installer", "  <- 安装程序")
I18N(S_PKGI_HINT,     "run 'install <file.lst>' to install it",
                      "运行 install <文件.lst> 来安装它")
I18N(S_RUN_NOMEM,     "run: out of memory", "run: 内存不足")
I18N(S_RUN_NOFILE,    "run: %s: no such file", "run: %s: 文件不存在")
I18N(S_RUN_START,     "running %s in ring 3 (%d bytes)",
                      "正在 ring 3 运行 %s (%d 字节)")
I18N(S_RUN_EXIT,      "process exited with status %d", "进程已退出, 状态码 %d")

/* ---- terminal: memory / ACPI / devices ------------------------------- */
I18N(S_MEMTEST_ALLOC,   "allocating %d MiB (0x%x bytes)...",
                        "正在分配 %d MiB (0x%x 字节)...")
I18N(S_MEMTEST_NOALLOC, "allocation failed - heap %d KiB used of %d MiB",
                        "分配失败 - 堆已用 %d KiB / %d MiB")
I18N(S_MEMTEST_MISMATCH,"MISMATCH at +0x%x", "地址 +0x%x 处校验失败")
I18N(S_MEMTEST_LAYOUT,  "heap base 0x%x, block at 0x%x (+%d MiB into the heap)",
                        "堆起始 0x%x, 内存块 0x%x (堆内偏移 +%d MiB)")
I18N(S_MEMTEST_VERIFY,  "every 4 KiB page written and read back: %s",
                        "每 4 KiB 页写入并读回: %s")
I18N(S_MEMTEST_HEAP,    "heap now: %d KiB used of %d MiB",
                        "堆当前: 已用 %d KiB / %d MiB")
I18N(S_PASS,            "OK", "通过")
I18N(S_FAILED,          "FAILED", "失败")

I18N(S_ACPI_NONE,   "ACPI: not available", "ACPI: 不可用")
I18N(S_ACPI_REV,    "ACPI revision : %d (%s)", "ACPI 版本    : %d (%s)")
I18N(S_ACPI_OEM,    "OEM id        : '%s'  table id '%s'",
                    "OEM 标识     : '%s'  表标识 '%s'")
I18N(S_ACPI_TABLES, "tables        : %d", "ACPI 表数量  : %d")
I18N(S_ACPI_RSDT,   "RSDT          : 0x%x", "RSDT         : 0x%x")
I18N(S_ACPI_XSDT,   "XSDT          : 0x%x", "XSDT         : 0x%x")
I18N(S_ACPI_DSDT,   "DSDT          : 0x%x", "DSDT         : 0x%x")
I18N(S_ACPI_CPUS,   "cpus          : %d (lapic at 0x%x, %d ioapic)",
                    "处理器       : %d 个 (lapic 位于 0x%x, %d 个 ioapic)")
I18N(S_ACPI_PM1,    "PM1a_CNT_BLK  : 0x%x   PM1b 0x%x",
                    "PM1a_CNT_BLK : 0x%x   PM1b 0x%x")
I18N(S_ACPI_FACS,   "found FACS    : 0x%x (%d bytes)",
                    "找到 FACS    : 0x%x (%d 字节)")
I18N(S_ACPI_S5,     "poweroff works through ACPI S5",
                    "关机通过 ACPI S5 实现")

I18N(S_DEV_NONE,   "no block devices", "没有块设备")
I18N(S_DEV_NAME,   "NAME", "名称")
I18N(S_DEV_TYPE,   "TYPE", "类型")
I18N(S_DEV_SECTORS,"SECTORS", "扇区数")
I18N(S_DEV_SIZE,   "SIZE", "容量")
I18N(S_DEV_MODEL,  "MODEL", "型号")

I18N(S_MOUNTS_POINT, "POINT", "挂载点")
I18N(S_MOUNTS_DEV,   "DEV", "设备")
I18N(S_MOUNTS_TYPE,  "TYPE", "类型")
I18N(S_MOUNTS_TOTAL, "TOTAL", "总量")
I18N(S_MOUNTS_FREE,  "FREE", "剩余")
I18N(S_MOUNTS_LABEL, "LABEL", "卷标")
I18N(S_MOUNT_OK,     "mounted %s on %s", "已挂载 %s 到 %s")
I18N(S_MOUNT_NODEV,  "mount: %s: no such device (try 'dev')",
                     "mount: %s: 设备不存在 (试试 dev)")
I18N(S_MOUNT_ABS,    "mount: mount point must be an absolute path",
                     "mount: 挂载点必须是绝对路径")
I18N(S_MOUNT_BUSY,   "mount: %s: mount point already in use",
                     "mount: %s: 挂载点已被占用")
I18N(S_MOUNT_NOFS,   "mount: %s: no MINIFS or ISO9660 filesystem found",
                     "mount: %s: 未找到 MINIFS 或 ISO9660 文件系统")
I18N(S_MOUNT_ALREADY,"mount: %s: device is already mounted",
                     "mount: %s: 设备已经挂载")
I18N(S_MOUNT_FAIL,   "mount: failed (%d)", "mount: 失败 (%d)")
I18N(S_UMOUNT_OK,    "unmounted %s", "已卸载 %s")
I18N(S_UMOUNT_ROOT,  "umount: cannot unmount the root filesystem",
                     "umount: 无法卸载根文件系统")
I18N(S_UMOUNT_NOMOUNT,"umount: %s: not mounted", "umount: %s: 未挂载")
I18N(S_MKFS_WARN,    "formatting %s, this erases everything on it...",
                     "正在格式化 %s, 这会清空它的所有数据...")
I18N(S_MKFS_OK,      "%s formatted as MINIFS02", "%s 已格式化为 MINIFS02")
I18N(S_MKFS_NODEV,   "mkfs: %s: no such device", "mkfs: %s: 设备不存在")
I18N(S_MKFS_RO,      "mkfs: %s: read-only or unsupported device",
                     "mkfs: %s: 只读或不支持的设备")
I18N(S_MKFS_UNMOUNT, "mkfs: %s: unmount it first", "mkfs: %s: 请先卸载它")
I18N(S_MKFS_FAIL,    "mkfs: failed (%d)", "mkfs: 失败 (%d)")

/* ---- terminal: gfx --------------------------------------------------- */
I18N(S_GFX_DISPLAY, "display : %dx%d, %d bpp", "显示器 : %dx%d, %d 位色")
I18N(S_GFX_FB,      "fb      : 0x%x, pitch %d", "帧缓冲 : 0x%x, 行距 %d")

/* ---- boot / setup messages on the terminal --------------------------- */
I18N(S_BOOT_SETUP_DISC, "aptos setup disc detected", "检测到 aptos 安装光盘")
I18N(S_BOOT_NO_SETUP,   "SETUP.LST not found on the disc", "光盘上没有找到 SETUP.LST")
I18N(S_BOOT_NO_CD,      "cannot mount the CD-ROM", "无法挂载光驱")
I18N(S_BOOT_SETUP_EXIT, "setup exited with status %d", "安装程序已退出, 状态码 %d")

/* ---- terminal: network ------------------------------------------------ */
I18N(S_NET_NONE,     "network: no e1000 NIC found", "网络: 未找到 e1000 网卡")
I18N(S_NET_PROBING,  "dhcp: asking the network...", "dhcp: 正在请求网络配置...")
I18N(S_NET_NOIP,     "no IP address - run 'dhcp' first", "没有 IP 地址 - 请先运行 dhcp")
I18N(S_NET_IFCONFIG, "%s\n  ip/mask : %s/%s\n  gateway : %s\n  dns     : %s\n  dhcp svr: %s",
                     "%s\n  地址/掩码: %s/%s\n  网关    : %s\n  DNS     : %s\n  DHCP    : %s")
I18N(S_NET_LINKDOWN, "link down or no address", "链路未连接或未配置地址")
I18N(S_NET_DHCP_OK,  "dhcp: leased %s/%s via %s (DNS %s)", "dhcp: 获得地址 %s/%s (DNS %s), 服务器 %s")
I18N(S_NET_DHCP_FAIL,"dhcp: no offer within %d ms", "dhcp: %d 毫秒内没有收到响应")
I18N(S_NET_PING_OK,  "ping %s: reply in %d ms", "ping %s: 应答, 延迟 %d 毫秒")
I18N(S_NET_PING_TMO, "ping %s: no reply", "ping %s: 无应答")
I18N(S_NET_PING_RES, "ping %s: cannot resolve the hardware address", "ping %s: 无法解析硬件地址")
I18N(S_NET_PING_NOGW, "ping %s: off-subnet and no gateway configured (run dhcp)",
     "ping %s: 目标不在本子网且没有网关（请先执行 dhcp）")
I18N(S_NET_PING_BADIP,"ping: not a valid IPv4 address", "ping: IPv4 地址不合法")
I18N(S_NET_DNS_OK,   "%s -> %s", "%s -> %s")
I18N(S_NET_DNS_FAIL, "nslookup %s: failed (%d)", "nslookup %s: 失败 (%d)")
I18N(S_NET_DNS_NOIP, "nslookup: no DNS server configured", "nslookup: 未配置 DNS 服务器")

/* ---- terminal: audio -------------------------------------------------- */
I18N(S_AUDIO_NONE,   "audio: no Intel HDA device found", "音频: 未找到 Intel HDA 设备")
I18N(S_AUDIO_INFO,   "audio   : %s\n  codec  : %d\n  gain   : %d steps\n  format : 48 kHz, 16 bit, stereo",
                     "音频设备 : %s\n  编解码器: %d\n  增益   : %d 级\n  格式   : 48 kHz, 16 位, 立体声")
I18N(S_AUDIO_BEEP,   "beep: %d Hz for %d ms", "beep: %d 赫兹, 持续 %d 毫秒")
I18N(S_AUDIO_MUSIC,  "playing a little tune...", "正在播放一段小曲...")
I18N(S_AUDIO_PLAYERR,"audio: playback failed (%d)", "音频: 播放失败 (%d)")
