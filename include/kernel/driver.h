#ifndef _KERNEL_DRIVER_H
#define _KERNEL_DRIVER_H

#include <uapi/types.h>

#define DRIVER_NAME_MAX 32
#define DRIVER_MAX      32

typedef enum {
    DRIVER_TYPE_SERIAL,
    DRIVER_TYPE_DISPLAY,
    DRIVER_TYPE_IRQCHIP,
    DRIVER_TYPE_TIMER,
    DRIVER_TYPE_MAILBOX,
    DRIVER_TYPE_OTHER
} driver_type_t;

typedef struct driver {
    char          name[DRIVER_NAME_MAX];
    driver_type_t type;
    int           (*init)(void);
    int           (*read)(void *buf, size_t len);
    int           (*write)(const void *buf, size_t len);
    int           (*ioctl)(uint32_t cmd, void *arg);
    int           initialized;
} driver_t;

int      driver_register(driver_t *drv);
driver_t *driver_find(const char *name);
driver_t *driver_find_by_type(driver_type_t type);
void      driver_list_all(void);
int       driver_count(void);
driver_t *driver_get(int index);

#endif
