BITS 32

global user_enter
global user_exit_asm
global user_exit_status
global user_set_kernel_stack

section .bss
align 4
user_kernel_esp:  resd 1
user_kernel_stack: resd 1
user_exit_status: resd 1

section .text

user_enter:
        mov     eax, [esp + 4]          ; entry point
        mov     edx, [esp + 8]          ; user stack top

        ; Remember where the kernel must resume when the program exits.
        mov     [user_kernel_esp], esp

        ; Switch to the dedicated kernel stack for the duration of the user
        ; program.  Running on the caller's stack is what made deep syscalls
        ; (reading a large file, with timer interrupts landing on top)
        ; overflow it: the caller is a terminal command handler that already
        ; occupies a good part of the stack.  user_exit_asm() restores esp
        ; from user_kernel_esp, so the caller's stack is untouched.
        mov     esp, [user_kernel_stack]
        test    esp, esp
        jz      .on_caller_stack
        xor     ebp, ebp

.on_caller_stack:
        mov     cx, 0x23
        mov     ds, cx
        mov     es, cx
        mov     fs, cx
        mov     gs, cx

        push    dword 0x23
        push    edx
        pushfd
        or      dword [esp], 0x200
        push    dword 0x1B
        push    eax
        iret

user_exit_asm:
        mov     eax, [esp + 4]
        mov     [user_exit_status], eax

        ; Restore the kernel data segments before going back to C code.
        ; user_enter() switched ds/es/fs/gs to the ring 3 selector (0x23)
        ; and never switched them back, so anything the kernel touched
        ; after the ret below was addressed through the wrong descriptor.
        mov     cx, 0x10
        mov     ds, cx
        mov     es, cx
        mov     fs, cx
        mov     gs, cx

        mov     esp, [user_kernel_esp]
        sti
        ret

; Called once by the kernel with the top of a private kernel stack that ring 3
; programs run on.
user_set_kernel_stack:
        mov     eax, [esp + 4]
        mov     [user_kernel_stack], eax
        ret
