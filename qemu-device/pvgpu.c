/*
 * PVGPU - Paravirtualized GPU device for QEMU
 *
 * This device provides GPU acceleration for Windows guests by:
 * 1. Presenting a PCIe device with shared memory BAR
 * 2. Forwarding commands to a host backend service
 * 3. Handling interrupts for synchronization
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "sysemu/sysemu.h"

#include "pvgpu.h"

/* Include shared protocol definitions */
#include "../../protocol/pvgpu_protocol.h"

#define TYPE_PVGPU "pvgpu"
OBJECT_DECLARE_SIMPLE_TYPE(PvgpuState, PVGPU)

/* MSI-X configuration */
#define PVGPU_MSIX_VECTORS  2
#define PVGPU_MSIX_BAR      1  /* BAR1 for MSI-X table */

struct PvgpuState {
    PCIDevice parent_obj;
    
    /* Configuration */
    uint32_t shmem_size;           /* Shared memory size (configurable) */
    char *backend_pipe;            /* Named pipe path for backend */
    
    /* BAR0: Control registers */
    MemoryRegion bar0;
    uint32_t status;
    uint32_t irq_status;
    uint32_t irq_mask;
    
    /* BAR2: Shared memory */
    MemoryRegion bar2;
    void *shmem;                   /* Pointer to shared memory */
    PvgpuControlRegion *ctrl;      /* Control region at start of shmem */
    
    /* Backend communication */
    /* TODO: Named pipe handle / socket */
    bool backend_connected;
    
    /* MSI-X */
    bool msix_enabled;
};

/*
 * =============================================================================
 * BAR0 Register Access (Control/Config)
 * =============================================================================
 */

static uint64_t pvgpu_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    PvgpuState *s = PVGPU(opaque);
    uint64_t val = 0;
    
    switch (addr) {
    case PVGPU_REG_VERSION:
        val = PVGPU_VERSION;
        break;
    case PVGPU_REG_FEATURES:
        val = (uint32_t)(PVGPU_FEATURES_MVP & 0xFFFFFFFF);
        break;
    case PVGPU_REG_FEATURES_HI:
        val = (uint32_t)(PVGPU_FEATURES_MVP >> 32);
        break;
    case PVGPU_REG_STATUS:
        val = s->status;
        if (s->backend_connected) {
            val |= PVGPU_STATUS_BACKEND_CONN;
        }
        break;
    case PVGPU_REG_IRQ_STATUS:
        val = s->irq_status;
        break;
    case PVGPU_REG_IRQ_MASK:
        val = s->irq_mask;
        break;
    case PVGPU_REG_SHMEM_SIZE:
        val = s->shmem_size;
        break;
    case PVGPU_REG_RING_SIZE:
        val = PVGPU_COMMAND_RING_SIZE;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "pvgpu: read from unknown register 0x%lx\n", 
                      (unsigned long)addr);
        break;
    }
    
    return val;
}

static void pvgpu_bar0_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    PvgpuState *s = PVGPU(opaque);
    
    switch (addr) {
    case PVGPU_REG_STATUS:
        /* Status is mostly read-only, but guest can clear error */
        s->status &= ~(val & PVGPU_STATUS_ERROR);
        break;
    case PVGPU_REG_DOORBELL:
        /* Guest is notifying us of new commands */
        /* TODO: Signal the backend service */
        break;
    case PVGPU_REG_IRQ_STATUS:
        /* Write 1 to clear */
        s->irq_status &= ~val;
        break;
    case PVGPU_REG_IRQ_MASK:
        s->irq_mask = val;
        break;
    case PVGPU_REG_RESET:
        if (val == 1) {
            /* Reset device state */
            s->status = PVGPU_STATUS_READY;
            s->irq_status = 0;
            if (s->ctrl) {
                s->ctrl->producer_ptr = 0;
                s->ctrl->consumer_ptr = 0;
                s->ctrl->guest_fence_request = 0;
                s->ctrl->host_fence_completed = 0;
            }
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "pvgpu: write to unknown register 0x%lx\n",
                      (unsigned long)addr);
        break;
    }
}

static const MemoryRegionOps pvgpu_bar0_ops = {
    .read = pvgpu_bar0_read,
    .write = pvgpu_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/*
 * =============================================================================
 * Device Initialization
 * =============================================================================
 */

static void pvgpu_init_shmem(PvgpuState *s)
{
    /* Initialize control region */
    s->ctrl = (PvgpuControlRegion *)s->shmem;
    memset(s->ctrl, 0, PVGPU_CONTROL_REGION_SIZE);
    
    s->ctrl->magic = PVGPU_MAGIC;
    s->ctrl->version = PVGPU_VERSION;
    s->ctrl->features = PVGPU_FEATURES_MVP;
    
    /* Ring starts after control region */
    s->ctrl->ring_offset = PVGPU_CONTROL_REGION_SIZE;
    s->ctrl->ring_size = PVGPU_COMMAND_RING_SIZE;
    
    /* Heap starts after ring */
    s->ctrl->heap_offset = PVGPU_CONTROL_REGION_SIZE + PVGPU_COMMAND_RING_SIZE;
    s->ctrl->heap_size = s->shmem_size - s->ctrl->heap_offset;
    
    /* Default display settings */
    s->ctrl->display_width = 1920;
    s->ctrl->display_height = 1080;
    s->ctrl->display_refresh = 60;
    s->ctrl->display_format = 87;  /* DXGI_FORMAT_B8G8R8A8_UNORM */
}

static void pvgpu_realize(PCIDevice *pci_dev, Error **errp)
{
    PvgpuState *s = PVGPU(pci_dev);
    
    /* Validate configuration */
    if (s->shmem_size < 64 * MiB) {
        error_setg(errp, "pvgpu: shmem_size must be at least 64MB");
        return;
    }
    
    /* Initialize BAR0: Control registers (4KB) */
    memory_region_init_io(&s->bar0, OBJECT(s), &pvgpu_bar0_ops, s,
                          "pvgpu-bar0", PVGPU_BAR0_SIZE);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    
    /* Initialize BAR2: Shared memory */
    s->shmem = g_malloc0(s->shmem_size);
    if (!s->shmem) {
        error_setg(errp, "pvgpu: failed to allocate shared memory");
        return;
    }
    
    memory_region_init_ram_ptr(&s->bar2, OBJECT(s), "pvgpu-bar2",
                               s->shmem_size, s->shmem);
    pci_register_bar(pci_dev, 2, 
                     PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->bar2);
    
    /* Initialize shared memory with control region */
    pvgpu_init_shmem(s);
    
    /* Initialize MSI-X if available */
    if (msix_init_exclusive_bar(pci_dev, PVGPU_MSIX_VECTORS, PVGPU_MSIX_BAR, errp) == 0) {
        s->msix_enabled = true;
    } else {
        /* Fall back to legacy IRQ */
        s->msix_enabled = false;
        error_free(*errp);
        *errp = NULL;
    }
    
    /* Mark device as ready */
    s->status = PVGPU_STATUS_READY;
    
    /* TODO: Connect to backend service */
    s->backend_connected = false;
}

static void pvgpu_exit(PCIDevice *pci_dev)
{
    PvgpuState *s = PVGPU(pci_dev);
    
    if (s->msix_enabled) {
        msix_uninit_exclusive_bar(pci_dev);
    }
    
    if (s->shmem) {
        g_free(s->shmem);
        s->shmem = NULL;
    }
}

static void pvgpu_reset(DeviceState *dev)
{
    PvgpuState *s = PVGPU(dev);
    
    s->status = PVGPU_STATUS_READY;
    s->irq_status = 0;
    s->irq_mask = 0;
    
    if (s->ctrl) {
        pvgpu_init_shmem(s);
    }
}

/*
 * =============================================================================
 * Interrupt Handling
 * =============================================================================
 */

void pvgpu_raise_irq(PvgpuState *s, uint32_t irq_bits)
{
    s->irq_status |= irq_bits;
    
    if (s->irq_status & s->irq_mask) {
        if (s->msix_enabled) {
            msix_notify(PCI_DEVICE(s), 0);
        } else {
            pci_irq_assert(PCI_DEVICE(s));
        }
    }
}

/*
 * =============================================================================
 * Device Properties and Registration
 * =============================================================================
 */

static Property pvgpu_properties[] = {
    DEFINE_PROP_UINT32("shmem_size", PvgpuState, shmem_size, PVGPU_DEFAULT_SHMEM_SIZE),
    DEFINE_PROP_STRING("backend_pipe", PvgpuState, backend_pipe),
    DEFINE_PROP_END_OF_LIST(),
};

static void pvgpu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    
    k->realize = pvgpu_realize;
    k->exit = pvgpu_exit;
    k->vendor_id = PVGPU_VENDOR_ID;
    k->device_id = PVGPU_DEVICE_ID;
    k->revision = PVGPU_REVISION;
    k->class_id = PVGPU_PCI_CLASS;
    k->subsystem_vendor_id = PVGPU_SUBSYSTEM_VENDOR_ID;
    k->subsystem_id = PVGPU_SUBSYSTEM_ID;
    
    dc->reset = pvgpu_reset;
    dc->desc = "Paravirtualized GPU Device";
    device_class_set_props(dc, pvgpu_properties);
    
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo pvgpu_info = {
    .name = TYPE_PVGPU,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PvgpuState),
    .class_init = pvgpu_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void pvgpu_register_types(void)
{
    type_register_static(&pvgpu_info);
}

type_init(pvgpu_register_types)
