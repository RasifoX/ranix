#include <kernel/driver.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>

static driver_t *driver_list = NULL;
static int       driver_cnt  = 0;

static const char *type_name(driver_type_t t)
{
    static const char *names[] = {
        "serial", "display", "irqchip", "timer", "mailbox", "block", "other"
    };
    return names[t < 7 ? t : 6];
}

int driver_register(driver_t *drv)
{
    if (drv->init) {
        int ret = drv->init();
        drv->initialized = (ret == 0) ? 1 : 0;
        if (ret != 0) {
            kprintf("%s: init failed (%d)\n", drv->name, ret);
            return ret;
        }
    } else {
        drv->initialized = 1;
    }

    drv->next   = driver_list;
    driver_list = drv;
    driver_cnt++;

    kprintf("%s: %s\n", drv->name, type_name(drv->type));
    return 0;
}

driver_t *driver_find(const char *name)
{
    for (driver_t *d = driver_list; d; d = d->next)
        if (strcmp(d->name, name) == 0)
            return d;
    return NULL;
}

driver_t *driver_find_by_type(driver_type_t type)
{
    for (driver_t *d = driver_list; d; d = d->next)
        if (d->type == type)
            return d;
    return NULL;
}

void driver_list_all(void)
{
    for (driver_t *d = driver_list; d; d = d->next)
        kprintf("  %s  %s  %s\n", d->name, type_name(d->type),
                d->initialized ? "ok" : "fail");
}

int driver_count(void) { return driver_cnt; }

driver_t *driver_get(int index)
{
    int i = 0;
    for (driver_t *d = driver_list; d; d = d->next, i++)
        if (i == index)
            return d;
    return NULL;
}
