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

# void enter_user_mode(uint32_t entry_point)
enter_user_mode:
    cli

    # Segmentleri User Data (0x23) ile hazırla
    mov $0x23, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    # --- IRET Stack Frame Hazırlığı ---
    
    # 1. SS (Stack Segment) -> User Data (0x23)
    push $0x23

    # 2. ESP (User Stack Pointer)
    # Şimdilik kernel stack'in o anki halini user stack gibi kullanıyoruz (HACK)
    # push ss (4 byte) yapmadan önceki ESP değerini hesaplıyoruz.
    mov %esp, %eax
    add $4, %eax 
    push %eax

    # 3. EFLAGS
    pushf
    pop %eax
    or $0x200, %eax  # IF (Interrupt Enable) bayrağını aç
    push %eax

    # 4. CS (Code Segment) -> User Code (0x1B)
    push $0x1B

    # 5. EIP (Entry Point)
    # Stack Yapısı: [CS, EFLAGS, ESP, SS, RetAddr, Argüman]
    # Offsetler:     0      4      8   12     16      20
    mov 20(%esp), %eax
    push %eax

    # Zıpla!
    iret