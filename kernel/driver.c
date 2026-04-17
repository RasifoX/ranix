#include <kernel/driver.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>

static driver_t *drivers[DRIVER_MAX];
static int driver_cnt = 0;

static const char *type_name(driver_type_t t)
{
    static const char *names[] = {
        "serial", "display", "irqchip", "timer", "mailbox", "other"
    };
    return names[t < 6 ? t : 5];
}

int driver_register(driver_t *drv)
{
    if (driver_cnt >= DRIVER_MAX)
        return -1;

    drivers[driver_cnt++] = drv;

    if (drv->init) {
        int ret = drv->init();
        drv->initialized = (ret == 0) ? 1 : 0;
        if (ret != 0) {
            kprintf("[  drv] %s: init failed (%d)\n", drv->name, ret);
            return ret;
        }
    } else {
        drv->initialized = 1;
    }

    kprintf("[  drv] %s %s\n", drv->name, type_name(drv->type));
    return 0;
}

driver_t *driver_find(const char *name)
{
    for (int i = 0; i < driver_cnt; i++)
        if (strcmp(drivers[i]->name, name) == 0)
            return drivers[i];
    return 0;
}

driver_t *driver_find_by_type(driver_type_t type)
{
    for (int i = 0; i < driver_cnt; i++)
        if (drivers[i]->type == type)
            return drivers[i];
    return 0;
}

void driver_list_all(void)
{
    for (int i = 0; i < driver_cnt; i++) {
        driver_t *d = drivers[i];
        kprintf("  %s  %s  %s\n",
                d->name, type_name(d->type),
                d->initialized ? "ok" : "fail");
    }
}

int driver_count(void) { return driver_cnt; }

driver_t *driver_get(int index)
{
    if (index < 0 || index >= driver_cnt)
        return 0;
    return drivers[index];
}
