.section .text
.global gdt_flush
.type gdt_flush, @function

gdt_flush:
    mov 4(%esp), %eax

    lgdt (%eax)

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    jmp $0x08, $.flush

.flush:
    ret


.global tss_flush
.type tss_flush, @function

tss_flush:
    
    mov $0x28, %ax
    ltr %ax
    ret

.global enter_user_mode
.type enter_user_mode, @function

enter_user_mode:
    mov 4(%esp), %ecx

    mov $0x23, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push $0x23

    push %esp 

    pushf

    pop %eax
    or $0x200, %eax
    push %eax

    push $0x1B

    push %ecx

    iret