; 最小长模式测试：从 16 位实模式自举 —— 建 GDT、进保护模式、
; 建四级页表、进长模式，然后在 64 位下用串口输出。
; 全部自包含，不依赖任何外部状态。
BITS 16
ORG 0x8000

%define PML4 0x1000
%define PDPT 0x2000
%define PD   0x3000

entry16:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x7C00
        cld

        ; ---- 串口初始化（16 位下用 mov dx, imm16）----
        mov     dx, 0x3F9
        xor     al, al
        out     dx, al
        mov     dx, 0x3FB
        mov     al, 0x80
        out     dx, al
        mov     dx, 0x3F8
        mov     al, 1
        out     dx, al
        mov     dx, 0x3F9
        xor     al, al
        out     dx, al
        mov     dx, 0x3FB
        mov     al, 3
        out     dx, al
        mov     dx, 0x3FA
        mov     al, 0xC7
        out     dx, al
        mov     dx, 0x3FC
        mov     al, 0x0B
        out     dx, al

        mov     si, msg_rm
        call    puts16

        ; ---- 进入 32 位保护模式 ----
        lgdt    [gdt_ptr]
        mov     eax, cr0
        or      eax, 1
        mov     cr0, eax
        jmp     dword 0x08:pm_entry

puts16:
        lodsb
        test    al, al
        jz      .d
        call    putc16
        jmp     puts16
.d:     ret
putc16:
        push    ax
        push    dx
        mov     dx, 0x3F8
        out     dx, al
        mov     dx, 0x3FD
        mov     cx, 0xFFFF
.w:     in      al, dx
        test    al, 0x20
        jnz     .o
        loop    .w
.o:     pop     dx
        pop     ax
        ret

BITS 32
pm_entry:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, 0x7C00

        mov     esi, msg_pm
        call    puts32

        ; ---- 清空页表区 ----
        mov     edi, PML4
        mov     ecx, 4096 * 3 / 4
        xor     eax, eax
        rep     stosd

        ; PML4[0] -> PDPT
        mov     eax, PDPT
        or      eax, 0x03
        mov     [PML4], eax

        ; PDPT[0] -> PD
        mov     eax, PD
        or      eax, 0x03
        mov     [PDPT], eax

        ; PD: 2MB 大页，恒等映射前 1GB
        mov     edi, PD
        mov     eax, 0x83
        mov     ecx, 512
.fill:
        mov     [edi], eax
        add     eax, 0x200000
        add     edi, 8
        loop    .fill

        ; --- 检查 CPUID 是否报告长模式 ---
        mov     eax, 0x80000000
        cpuid
        cmp     eax, 0x80000001
        jb      .nolm

        mov     eax, 0x80000001
        cpuid
        push    edx
        mov     esi, msg_ext
        call    puts32
        pop     edx
        mov     ebx, edx
        call    hex32
        mov     esi, crlf
        call    puts32
        test    edx, (1 << 29)          ; LM 位
        jz      .nolm
        mov     esi, msg_haslm
        call    puts32
        jmp     .lmok
.nolm:
        mov     esi, msg_nolm
        call    puts32
.lmok:

        mov     eax, PML4
        mov     cr3, eax

        mov     eax, cr4
        or      eax, (1 << 5)        ; PAE
        mov     cr4, eax

        ; --- 调试：读 EFER 原值并打印 ---
        mov     ecx, 0xC0000080
        rdmsr
        push    eax
        push    edx
        mov     esi, msg_efer1
        call    puts32
        pop     edx
        pop     eax
        push    eax
        mov     ebx, eax
        call    hex32
        mov     esi, crlf
        call    puts32
        pop     eax

        or      eax, (1 << 8)        ; EFER.LME
        mov     ecx, 0xC0000080
        wrmsr

        ; --- 调试：回读 EFER ---
        mov     ecx, 0xC0000080
        rdmsr
        push    eax
        mov     esi, msg_efer2
        call    puts32
        pop     eax
        mov     ebx, eax
        call    hex32
        mov     esi, crlf
        call    puts32

        mov     eax, cr0
        or      eax, 0x80000000      ; CR0.PG
        mov     cr0, eax

        jmp     0x18:lm_entry        ; 远跳进 64 位代码段

puts32:
        lodsb
        test    al, al
        jz      .d
        call    putc32
        jmp     puts32
.d:     ret
putc32:
        push    ax
        push    dx
        mov     dx, 0x3F8
        out     dx, al
        mov     dx, 0x3FD
        mov     cx, 0xFFFF
.w:     in      al, dx
        test    al, 0x20
        jnz     .o
        loop    .w
.o:     pop     dx
        pop     ax
        ret

hex32:
        push    eax
        push    ecx
        push    edx
        mov     ecx, 8
.h:
        rol     ebx, 4
        mov     al, bl
        and     al, 0x0F
        cmp     al, 10
        jb      .dig
        add     al, 'a' - 10
        jmp     .out
.dig:   add     al, '0'
.out:   mov     dx, 0x3F8
        out     dx, al
        mov     dx, 0x3FD
        push    cx
        mov     cx, 0xFFFF
.w:     in      al, dx
        test    al, 0x20
        jnz     .o
        loop    .w
.o:     pop     cx
        loop    .h
        pop     edx
        pop     ecx
        pop     eax
        ret

msg_ext   db "  CPUID 80000001 EDX = 0x", 0
msg_haslm db "  -> long mode SUPPORTED", 13, 10, 0
msg_nolm  db "  -> long mode NOT supported", 13, 10, 0
msg_efer1 db "  EFER before = 0x", 0
msg_efer2 db "  EFER after  = 0x", 0
crlf      db 13, 10, 0

msg_rm  db "[boot] real mode, building page tables...", 13, 10, 0
msg_pm  db "[boot] protected mode OK, enabling long mode...", 13, 10, 0

align 16
gdt_start:
        dq 0x0000000000000000
gdt_code32:                              ; 选择子 0x08
        dq 0x00CF9A000000FFFF
gdt_data:                                ; 选择子 0x10
        dq 0x00CF92000000FFFF
gdt_code64:                              ; 选择子 0x18
        dq 0x00AF9A000000FFFF
gdt_end:
gdt_ptr:
        dw gdt_end - gdt_start - 1
        dd gdt_start

BITS 64
lm_entry:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     rsp, 0x90000

        mov     rsi, msg_lm
        call    puts64
        mov     rsi, msg_ok
        call    puts64

        ; --- 证明真的在 64 位：用 64 位寄存器算一个 32 位放不下的值 ---
        mov     rax, 0x123456789ABCDEF0
        mov     rbx, rax
        shr     rbx, 32                  ; 取高 32 位
        mov     rsi, msg_hi
        call    puts64
        mov     rbx, rax
        shr     rbx, 32
        call    hex64win
        mov     rsi, crlf64
        call    puts64

.hang:
        hlt
        jmp     .hang

puts64:
        lodsb
        test    al, al
        jz      .d
        call    putc64
        jmp     puts64
.d:     ret
putc64:
        push    ax
        push    dx
        mov     dx, 0x3F8
        out     dx, al
        mov     dx, 0x3FD
        mov     cx, 0xFFFF
.w:     in      al, dx
        test    al, 0x20
        jnz     .o
        loop    .w
.o:     pop     dx
        pop     ax
        ret

hex64win:
        push    rax
        push    rcx
        push    rdx
        mov     ecx, 8
.h:     rol     ebx, 4
        mov     al, bl
        and     al, 0x0F
        cmp     al, 10
        jb      .dig
        add     al, 'a' - 10
        jmp     .out
.dig:   add     al, '0'
.out:   mov     dx, 0x3F8
        out     dx, al
        push    rcx
        mov     ecx, 0xFFFF
.w:     in      al, dx
        test    al, 0x20
        jnz     .o
        loop    .w
.o:     pop     rcx
        loop    .h
        pop     rdx
        pop     rcx
        pop     rax
        ret

crlf64  db 13, 10, 0
msg_hi  db "  rax high 32 bits = 0x", 0
msg_lm  db "[boot] LONG MODE ACTIVE - 64-bit code running", 13, 10, 0
msg_ok  db "[boot] x86-64 OK.", 13, 10, 0

        times 32768 - ($ - $$) db 0
