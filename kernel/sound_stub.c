/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Machines without a sound driver: /sys/kernel/volume reads 0 and ignores writes */
#include <kernel/sound.h>
__attribute__((weak)) int  sound_volume(void) { return 0; }
__attribute__((weak)) void sound_set_volume(int v) { (void)v; }
