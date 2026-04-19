bits 64
default rel

section .data
msg: db "hello from x86_64", 10
msglen equ $ - msg

section .text
global _start
_start:
    mov rax, 1          ; sys_write
    mov rdi, 1          ; stdout
    lea rsi, [msg]
    mov rdx, msglen
    syscall

    mov rax, 231        ; sys_exit_group
    xor rdi, rdi
    syscall
