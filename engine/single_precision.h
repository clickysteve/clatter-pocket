/*
 * single_precision.h - force-included (-include) to compute the LXR DSP's double-precision
 * arithmetic in single precision. Copyright (C) 2026 The CLATTER contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The firmware computes its envelopes (SlopeEg2.c) and amplitude smoothing (1PoleLp.c) in
 * double precision, which the LXR-02's Cortex-M7 does in hardware. On an FPU without double
 * precision every such operation is a software call, per sample and voice. This header is the
 * fallback: the C library headers are read first (they need the real double), then double
 * means float in the DSP files. Measured against CLATTER by `make compare-sp`
 * (COMPATIBILITY.md). Not used unless a build asks for it.
 */
#ifndef POCKET_SINGLE_PRECISION_H
#define POCKET_SINGLE_PRECISION_H
#include <stddef.h>
#include <stdint.h>
#define double float
#endif
