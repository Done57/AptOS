BITS 32

global _start
extern kmain
extern __bss_start
extern __bss_end

section .text.entry
_start:
        cli
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, stack_top

        mov     edi, __bss_start
        mov     ecx, __bss_end
        sub     ecx, edi
        xor     eax, eax
        rep     stosb

        call    kmain

.hang:
        cli
        hlt
        jmp     .hang

section .bss
align 16
stack_bottom:
        resb    65536
stack_top:
