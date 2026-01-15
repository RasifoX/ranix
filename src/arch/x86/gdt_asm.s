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
    # TSS Segment Selector: 5. index * 8 = 40 (0x28)
    # Ring 3 yetkisi için OR 0x03 yapabiliriz (0x2B), ama LTR için genelde 0x28 (RPL=0) yeterlidir.
    # Biz güvenli olan 0x2B (0x28 | 3) kullanalım ki User Mode erişebilsin diyeceğiz ama
    # LTR bir "privileged instruction" olduğu için kernel modda çalışır.
    
    mov $0x28, %ax
    ltr %ax
    ret