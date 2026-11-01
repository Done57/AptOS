/* aptos 64 位内核链接脚本。
 *
 * 与 32 位版本的差别：
 *   - 输出格式是 elf64-x86-64
 *   - 装载地址抬到 1MB（0x100000），避开实模式/BIOS 使用的低端内存
 *   - 段按页对齐，方便用 2MB 或 4KB 页直接映射
 *   - 收集 init_array，让 C++ 式构造函数（目前没有，但留着）能跑
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text ALIGN(4K) : {
        *(.text.entry)
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4K) : {
        *(.data)
        *(.data.*)
        *(.init_array)
        *(.init_array.*)
    }

    __bss_start = .;
    .bss ALIGN(4K) : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    __bss_end = .;

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
