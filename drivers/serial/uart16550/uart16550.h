#ifndef UART16550_H
#define UART16550_H

void uart16550_init(void);
void uart16550_putc(char c);
char uart16550_getc(void);
int  uart16550_has_data(void);
void uart16550_register(void);
void uart16550_irq_init(void);

#endif
