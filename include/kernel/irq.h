#ifndef IRQ_H
#define IRQ_H
#include <uapi/types.h>

typedef void (*irq_handler_t)(void);

void irq_init(void);
void irq_register(uint32_t irq, irq_handler_t handler);
void irq_handle(uint32_t irq);
uint64_t irq_stat(uint32_t irq, int cpu, uint64_t *handler);
void irq_dispatch(uint32_t irq);
/* Route a PCI device's MSI to a free vector and register h for it: the vector,
 * or <0 (no MSI on this architecture or device, or no vector left; poll instead). */
int  pci_msi_attach(uint8_t bus, uint8_t dev, uint8_t fn, irq_handler_t h);
void irq_enable(void);
void irq_disable(void);
uint64_t irq_save(void);
void irq_restore(uint64_t flags);

#endif