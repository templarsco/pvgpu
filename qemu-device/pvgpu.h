/*
 * PVGPU - Paravirtualized GPU device for QEMU
 *
 * Internal header file
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_PVGPU_H
#define HW_DISPLAY_PVGPU_H

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"

/* Forward declarations */
typedef struct PvgpuState PvgpuState;

/* Raise an interrupt to the guest */
void pvgpu_raise_irq(PvgpuState *s, uint32_t irq_bits);

#endif /* HW_DISPLAY_PVGPU_H */
