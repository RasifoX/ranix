.section .text
.align 4

.global idt_flush

idt_flush:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    push $0
    push $\num 
    jmp isr_common_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
    push $\num
    jmp isr_common_stub
.endm


isr_common_stub:
    pusha

    xor %eax, %eax
    mov %ds, %ax
    push %eax

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push %esp
    call isr_handler
    mov %eax, %esp

    pop %eax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    popa
    add $8, %esp
    iret

# Exceptions (0-31) from - https://wiki.osdev.org/Exceptions

ISR_NOERRCODE 0   # Division Error
ISR_NOERRCODE 1   # Debug
ISR_NOERRCODE 2   # Non-maskable Interrupt
ISR_NOERRCODE 3   # Breakpoint
ISR_NOERRCODE 4   # Overflow
ISR_NOERRCODE 5   # Bound Range Exceeded
ISR_NOERRCODE 6   # Invalid Opcode
ISR_NOERRCODE 7   # Device Not Available

ISR_ERRCODE   8   # Double Fault

ISR_NOERRCODE 9   # Coprocessor Segment Overrun

ISR_ERRCODE   10  # Invalid TSS
ISR_ERRCODE   11  # Segment Not Present
ISR_ERRCODE   12  # Stack-Segment Fault
ISR_ERRCODE   13  # General Protection Fault
ISR_ERRCODE   14  # Page Fault

ISR_NOERRCODE 15  # Reserved
ISR_NOERRCODE 16  # x87 Floating-Point Exception

ISR_ERRCODE   17  # Alignment Check

ISR_NOERRCODE 18  # Machine Check
ISR_NOERRCODE 19  # SIMD Floating-Point Exception
ISR_NOERRCODE 20  # Virtualization Exception

ISR_ERRCODE   21  # Control Protection Exception

ISR_NOERRCODE 22  # Reserved
ISR_NOERRCODE 23  # Reserved
ISR_NOERRCODE 24  # Reserved
ISR_NOERRCODE 25  # Reserved
ISR_NOERRCODE 26  # Reserved
ISR_NOERRCODE 27  # Reserved
ISR_NOERRCODE 28  # Hypervisor Injection Exception
ISR_ERRCODE   29  # VMM Communication Exception 	
ISR_ERRCODE   30  # Security Exception
ISR_NOERRCODE 31  # Reserved


# IRQ. IDT = IRQ + 32

ISR_NOERRCODE 32  # Timer 
ISR_NOERRCODE 33  # Keyboard
ISR_NOERRCODE 34  # Cascade (PIC2)
ISR_NOERRCODE 35  # COM2 (Serial)
ISR_NOERRCODE 36  # COM1 (Serial)
ISR_NOERRCODE 37  # LPT2 (Printer)
ISR_NOERRCODE 38  # Floppy Drive
ISR_NOERRCODE 39  # LPT1 (Printer)
ISR_NOERRCODE 40  # CMOS / RTC (Real Time Clock)
ISR_NOERRCODE 41  # Free
ISR_NOERRCODE 42  # Free
ISR_NOERRCODE 43  # Free
ISR_NOERRCODE 44  # PS/2 Mouse
ISR_NOERRCODE 45  # FPU/Coprocessor
ISR_NOERRCODE 46  # Primary ATA (Hard Drive)
ISR_NOERRCODE 47  # Secondary ATA