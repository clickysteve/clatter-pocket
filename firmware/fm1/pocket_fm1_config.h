/*
 * pocket_fm1_config.h - force-included (-include) into every CLATTER Pocket engine file of
 * the FM-1 build (LXR, bridge, engine, panel). Not used by the host build.
 *
 * Copyright (C) 2026 The CLATTER contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef POCKET_FM1_CONFIG_H
#define POCKET_FM1_CONFIG_H

/* The LXR's constant tables (102 KB) are stored packed and unpacked into RAM at start-up
 * (engine/lxr_tables_unpack.c): the firmware's wavetable.c and transientTables.c are not
 * compiled. Byte-identical tables; see BUILDING.md. */
#define POCKET_PACKED_TABLES 1
#define POCKET_FM1 1

/* CLATTER's shim defines sqrtf and sqrt as static inline wrappers of __builtin_sqrtf and
 * __builtin_sqrt. The PI32v2 FPU has no square root, so the builtin becomes a call to the
 * library function of the same name: inside the shim that is the wrapper itself, an
 * infinite recursion that LLVM deletes as undefined behaviour (the pan gains came out as
 * garbage; found by the pre-flash audit, PRE-FLASH-AUDIT.md). Renamed, the wrappers call
 * the external sqrtf (pocket_fm1_libm.c, correctly rounded); a sqrt (double) call would
 * fail to link instead of miscompiling. The firmware checks require sqrtf in SRAM. */
#define sqrtf pocket_sqrtf
#define sqrt pocket_sqrt

/* Event ring ordering. The SDK's own audio code uses csync as its barrier. ASSUMPTION: the
 * control task and the audio interrupt run on the same core of the AC791N (see HARDWARE.md,
 * open questions); if they do not, this must become a real cross-core barrier. */
#ifndef POCKET_BARRIER
#define POCKET_BARRIER() __asm__ volatile("csync" ::: "memory")
#endif

/* CLATTER's freestanding libm (lxr/bridge/libm.c), including its mem*, under its own names:
 * the engine files call exactly the code the host build and CLATTER run, and nothing can
 * collide with the SDK's libc.a. (Calls the compiler generates itself, such as struct
 * copies, still go to the SDK's memcpy/memset.) The half-float helpers are unused: the FM-1
 * build has no FX bus (engine/fx_off.c). */
#define exp pocket_exp
#define expf pocket_expf
#define log pocket_log
#define logf pocket_logf
#define pow pocket_pow
#define powf pocket_powf
#define tanh pocket_tanh
#define tanhf pocket_tanhf
#define sin pocket_sin
#define sinf pocket_sinf
#define cos pocket_cos
#define cosf pocket_cosf
#define fmod pocket_fmod
#define fmodf pocket_fmodf
#define memset pocket_memset
#define memcpy pocket_memcpy
#define memmove pocket_memmove
#define __extendhfsf2 pocket_unused_extendhfsf2
#define __truncsfhf2 pocket_unused_truncsfhf2

#endif
