/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef UAPI_RANIX_PRCTL_H
#define UAPI_RANIX_PRCTL_H
/* Ranix-only prctl options. 0x5258xxxx is the "RX" range; asess.h holds 0x52580001. */
#define PR_RANIX_UNAME_LINUX     0x52580010   /* arg2 1: uname/ostype say "Linux" for this process tree, 0: "Ranix" */
#define PR_RANIX_GET_UNAME_LINUX 0x52580011
#endif
