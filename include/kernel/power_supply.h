/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Power supplies (kernel/power_supply.c): what a driver knows about the mains
 * adapter and the battery, shown as /sys/class/power_supply/<name>/ the way
 * Linux lays it out (upower, desktops and scripts read these files). Nothing is
 * made up: a supply exists only when a driver read it from the hardware (a
 * battery only while it is in its bay), and a value it does not know is left out. */
#ifndef KERNEL_POWER_SUPPLY_H
#define KERNEL_POWER_SUPPLY_H
#ifdef HOST_TEST
#include <stdint.h>
#include <stddef.h>
#else
#include <uapi/types.h>
#endif

enum { PS_MAINS = 1, PS_BATTERY = 2 };
enum { PS_ST_UNKNOWN, PS_ST_CHARGING, PS_ST_DISCHARGING, PS_ST_NOT_CHARGING, PS_ST_FULL };
#define PS_UNSET (-1L)                  /* a value the driver does not know: no file */

typedef struct {
	char name[8];                       /* "AC", "BAT0" */
	int  type;                          /* PS_MAINS / PS_BATTERY */
	int  online;                        /* mains: 1 plugged in */
	int  present;                       /* battery: 1 in its bay */
	int  status;                        /* PS_ST_* */
	int  capacity;                      /* percent, -1 unknown */
	int  capacity_level_critical;       /* the hardware's own "critical" flag */
	int  energy;                        /* 1: *_now/full/design are energy (uWh), 0: charge (uAh) */
	long now, full, design;             /* uWh or uAh, PS_UNSET unknown */
	long voltage_now;                   /* uV */
	long current_now;                   /* uA */
	char technology[12];                /* "Li-ion", "" unknown */
} ps_t;

/* kernel/lib/ps_format.c: pure, so tests/host_power.c runs it on a host */
int ps_attr_count(const ps_t *p);
const char *ps_attr_name(const ps_t *p, int i);          /* NULL past the end */
int ps_attr_format(const ps_t *p, const char *attr, char *out, size_t cap);   /* bytes, -1 no such file */
int ps_attr_slot(const ps_t *p, const char *attr);                            /* stable id, -1 none */
int ps_attr_format_slot(const ps_t *p, int slot, char *out, size_t cap);
/* /proc/hwdiag's "power supply ..." line from n supplies (no newline): bytes */
int ps_diag_line(const ps_t *ps, int n, char *out, size_t cap);

#ifndef HOST_TEST
/* kernel/power_supply.c */
void power_supply_set(const ps_t *p);         /* add or update by name (driver's thread) */
void power_supply_remove(const char *name);   /* gone (a battery taken out): leaves the list */
int  power_supply_get(int idx, ps_t *out);     /* 0, or -1 past the end */
void power_lid_set(int open);                 /* the lid: 1 open, 0 closed */
int  power_lid_get(void);                      /* -1 unknown (no driver knows it) */
struct vfs_inode;
struct vfs_inode *power_supply_sysfs_root(void);   /* /sys/class/power_supply */
#endif
#endif
