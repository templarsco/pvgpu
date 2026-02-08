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
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "sysemu/sysemu.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include "pvgpu.h"

/* Include shared protocol definitions */
#include "../../protocol/pvgpu_protocol.h"

#define TYPE_PVGPU "pvgpu"
OBJECT_DECLARE_SIMPLE_TYPE(PvgpuState, PVGPU)

/* MSI-X configuration */
#define PVGPU_MSIX_VECTORS  2
#define PVGPU_MSIX_BAR      1  /* BAR1 for MSI-X table */

/*
 * =============================================================================
 * IPC Protocol Definitions (must match backend/src/ipc.rs)
 * =============================================================================
 */

/* Message types for QEMU <-> Backend IPC */
#define IPC_MSG_HANDSHAKE       1
#define IPC_MSG_HANDSHAKE_ACK   2
#define IPC_MSG_DOORBELL        3
#define IPC_MSG_IRQ             4
#define IPC_MSG_SHUTDOWN        5

/* IPC message header */
typedef struct {
    uint32_t msg_type;
    uint32_t payload_size;
} IpcMessageHeader;

/*
 * =============================================================================
 * Device State
 * =============================================================================
 */

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
    
    /* Shared memory file mapping (for backend access) */
#ifdef _WIN32
    HANDLE shmem_mapping;          /* Windows file mapping handle */
    char shmem_name[64];           /* Name for the file mapping */
#else
    int shmem_fd;                  /* POSIX shared memory fd */
    char shmem_name[64];           /* Name for shm_open */
#endif
    
    /* Backend communication */
#ifdef _WIN32
    HANDLE backend_pipe_handle;    /* Named pipe handle */
#else
    int backend_socket;            /* Unix socket fd */
#endif
    bool backend_connected;
    QemuThread backend_thread;     /* Thread for reading backend messages */
    bool backend_thread_running;
    
    /* Negotiated features */
    uint64_t features;
    
    /* MSI-X */
    bool msix_enabled;
};

/*
 * =============================================================================
 * Backend IPC Communication
 * =============================================================================
 */

#ifdef _WIN32

/* Connect to the backend service via named pipe */
static bool pvgpu_backend_connect(PvgpuState *s)
{
    const char *pipe_path = s->backend_pipe ? s->backend_pipe : "\\\\.\\pipe\\pvgpu";
    
    /* Try to connect to existing pipe */
    s->backend_pipe_handle = CreateFileA(
        pipe_path,
        GENERIC_READ | GENERIC_WRITE,
        0,              /* No sharing */
        NULL,           /* Default security */
        OPEN_EXISTING,
        0,              /* Default attributes */
        NULL            /* No template */
    );
    
    if (s->backend_pipe_handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            /* Pipe exists but busy, wait for it */
            if (!WaitNamedPipeA(pipe_path, 5000)) {
                error_report("pvgpu: timeout waiting for backend pipe");
                return false;
            }
            /* Try again */
            s->backend_pipe_handle = CreateFileA(
                pipe_path,
                GENERIC_READ | GENERIC_WRITE,
                0, NULL, OPEN_EXISTING, 0, NULL
            );
        }
        if (s->backend_pipe_handle == INVALID_HANDLE_VALUE) {
            error_report("pvgpu: failed to connect to backend pipe: %lu", GetLastError());
            return false;
        }
    }
    
    /* Set pipe to message mode */
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(s->backend_pipe_handle, &mode, NULL, NULL)) {
        error_report("pvgpu: failed to set pipe mode: %lu", GetLastError());
        CloseHandle(s->backend_pipe_handle);
        s->backend_pipe_handle = INVALID_HANDLE_VALUE;
        return false;
    }
    
    return true;
}

/* Send a message to the backend */
static bool pvgpu_backend_send(PvgpuState *s, uint32_t msg_type, 
                               const void *payload, uint32_t payload_size)
{
    if (s->backend_pipe_handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    IpcMessageHeader header = {
        .msg_type = msg_type,
        .payload_size = payload_size
    };
    
    DWORD written;
    
    /* Write header */
    if (!WriteFile(s->backend_pipe_handle, &header, sizeof(header), &written, NULL) ||
        written != sizeof(header)) {
        error_report("pvgpu: failed to send message header");
        return false;
    }
    
    /* Write payload if present */
    if (payload_size > 0 && payload) {
        if (!WriteFile(s->backend_pipe_handle, payload, payload_size, &written, NULL) ||
            written != payload_size) {
            error_report("pvgpu: failed to send message payload");
            return false;
        }
    }
    
    /* NOTE: FlushFileBuffers intentionally removed. Named pipe writes are
     * kernel-buffered and delivered in-order. Flushing on every doorbell
     * added 50-100+ µs of synchronous I/O per notification. The pipe's
     * internal buffer (4KB) is more than sufficient for our small messages. */
    return true;
}

/* Read a message from the backend */
static bool pvgpu_backend_recv(PvgpuState *s, uint32_t *msg_type,
                               void *payload, uint32_t *payload_size)
{
    if (s->backend_pipe_handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    IpcMessageHeader header;
    DWORD read;
    
    /* Read header */
    if (!ReadFile(s->backend_pipe_handle, &header, sizeof(header), &read, NULL) ||
        read != sizeof(header)) {
        return false;
    }
    
    *msg_type = header.msg_type;
    *payload_size = header.payload_size;
    
    /* Read payload if present */
    if (header.payload_size > 0 && payload) {
        if (!ReadFile(s->backend_pipe_handle, payload, header.payload_size, &read, NULL) ||
            read != header.payload_size) {
            return false;
        }
    }
    
    return true;
}

/* Disconnect from backend */
static void pvgpu_backend_disconnect(PvgpuState *s)
{
    if (s->backend_pipe_handle != INVALID_HANDLE_VALUE) {
        /* Send shutdown message */
        pvgpu_backend_send(s, IPC_MSG_SHUTDOWN, NULL, 0);
        CloseHandle(s->backend_pipe_handle);
        s->backend_pipe_handle = INVALID_HANDLE_VALUE;
    }
    s->backend_connected = false;
}

#else /* POSIX */

static bool pvgpu_backend_connect(PvgpuState *s)
{
    const char *socket_path = s->backend_pipe ? s->backend_pipe : "/tmp/pvgpu.sock";
    
    s->backend_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->backend_socket < 0) {
        error_report("pvgpu: failed to create socket");
        return false;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (connect(s->backend_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_report("pvgpu: failed to connect to backend socket");
        close(s->backend_socket);
        s->backend_socket = -1;
        return false;
    }
    
    return true;
}

static bool pvgpu_backend_send(PvgpuState *s, uint32_t msg_type,
                               const void *payload, uint32_t payload_size)
{
    if (s->backend_socket < 0) {
        return false;
    }
    
    IpcMessageHeader header = {
        .msg_type = msg_type,
        .payload_size = payload_size
    };
    
    if (write(s->backend_socket, &header, sizeof(header)) != sizeof(header)) {
        return false;
    }
    
    if (payload_size > 0 && payload) {
        if (write(s->backend_socket, payload, payload_size) != payload_size) {
            return false;
        }
    }
    
    return true;
}

static bool pvgpu_backend_recv(PvgpuState *s, uint32_t *msg_type,
                               void *payload, uint32_t *payload_size)
{
    if (s->backend_socket < 0) {
        return false;
    }
    
    IpcMessageHeader header;
    if (read(s->backend_socket, &header, sizeof(header)) != sizeof(header)) {
        return false;
    }
    
    *msg_type = header.msg_type;
    *payload_size = header.payload_size;
    
    if (header.payload_size > 0 && payload) {
        if (read(s->backend_socket, payload, header.payload_size) != header.payload_size) {
            return false;
        }
    }
    
    return true;
}

static void pvgpu_backend_disconnect(PvgpuState *s)
{
    if (s->backend_socket >= 0) {
        pvgpu_backend_send(s, IPC_MSG_SHUTDOWN, NULL, 0);
        close(s->backend_socket);
        s->backend_socket = -1;
    }
    s->backend_connected = false;
}

#endif /* _WIN32 */

/* Perform handshake with backend */
static bool pvgpu_backend_handshake(PvgpuState *s)
{
    /* Build handshake payload: shmem_size (u64) + shmem_name (string) */
    uint8_t payload[256];
    uint64_t size64 = s->shmem_size;
    memcpy(payload, &size64, sizeof(size64));
    
    size_t name_len = strlen(s->shmem_name);
    memcpy(payload + 8, s->shmem_name, name_len + 1);  /* Include null terminator */
    
    /* Send handshake */
    if (!pvgpu_backend_send(s, IPC_MSG_HANDSHAKE, payload, 8 + name_len + 1)) {
        error_report("pvgpu: failed to send handshake");
        return false;
    }
    
    /* Wait for acknowledgement */
    uint32_t msg_type;
    uint64_t features;
    uint32_t payload_size = sizeof(features);
    
    if (!pvgpu_backend_recv(s, &msg_type, &features, &payload_size)) {
        error_report("pvgpu: failed to receive handshake ack");
        return false;
    }
    
    if (msg_type != IPC_MSG_HANDSHAKE_ACK) {
        error_report("pvgpu: unexpected message type %u (expected handshake ack)", msg_type);
        return false;
    }
    
    /* Validate features - ensure backend supports at least D3D11 */
    if (payload_size >= sizeof(features) && features != 0) {
        uint64_t required = PVGPU_FEATURE_D3D11;
        if ((features & required) != required) {
            error_report("pvgpu: backend missing required D3D11 feature (features=0x%"PRIx64")", features);
            return false;
        }
        /* Store negotiated features in BAR0 registers */
        s->features = features;
        info_report("pvgpu: backend features negotiated: 0x%"PRIx64, features);
    } else {
        /* Backend didn't send features - assume MVP set */
        s->features = PVGPU_FEATURES_MVP;
        info_report("pvgpu: backend sent no features, assuming MVP set");
    }
    return true;
}

/* Send doorbell notification to backend */
static void pvgpu_notify_backend(PvgpuState *s)
{
    if (s->backend_connected) {
        pvgpu_backend_send(s, IPC_MSG_DOORBELL, NULL, 0);
    }
}

/* Thread for receiving messages from backend */
static void *pvgpu_backend_thread(void *opaque)
{
    PvgpuState *s = opaque;
    uint32_t msg_type;
    uint32_t vector;
    uint32_t payload_size;
    
    while (s->backend_thread_running && s->backend_connected) {
        payload_size = sizeof(vector);
        if (!pvgpu_backend_recv(s, &msg_type, &vector, &payload_size)) {
            /* Connection lost */
            s->backend_connected = false;
            s->status &= ~PVGPU_STATUS_BACKEND_CONN;
            s->status |= PVGPU_STATUS_ERROR;
            break;
        }
        
        switch (msg_type) {
        case IPC_MSG_IRQ:
            /* Backend requests IRQ to guest */
            qemu_mutex_lock_iothread();
            pvgpu_raise_irq(s, PVGPU_IRQ_FENCE);
            qemu_mutex_unlock_iothread();
            break;
        case IPC_MSG_SHUTDOWN:
            s->backend_connected = false;
            s->status &= ~PVGPU_STATUS_BACKEND_CONN;
            break;
        default:
            error_report("pvgpu: unknown message from backend: %u", msg_type);
            break;
        }
    }
    
    return NULL;
}

/*
 * =============================================================================
 * Shared Memory Setup
 * =============================================================================
 */

#ifdef _WIN32

static bool pvgpu_create_shmem_mapping(PvgpuState *s)
{
    /* Generate unique name for the file mapping */
    snprintf(s->shmem_name, sizeof(s->shmem_name), "Global\\pvgpu_shmem_%p", s);
    
    /* Create file mapping backed by page file */
    s->shmem_mapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* Use paging file */
        NULL,                   /* Default security */
        PAGE_READWRITE,
        (DWORD)(s->shmem_size >> 32),
        (DWORD)(s->shmem_size & 0xFFFFFFFF),
        s->shmem_name
    );
    
    if (s->shmem_mapping == NULL) {
        error_report("pvgpu: failed to create file mapping: %lu", GetLastError());
        return false;
    }
    
    /* Map the view */
    s->shmem = MapViewOfFile(
        s->shmem_mapping,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        s->shmem_size
    );
    
    if (s->shmem == NULL) {
        error_report("pvgpu: failed to map view: %lu", GetLastError());
        CloseHandle(s->shmem_mapping);
        s->shmem_mapping = NULL;
        return false;
    }
    
    return true;
}

static void pvgpu_destroy_shmem_mapping(PvgpuState *s)
{
    if (s->shmem) {
        UnmapViewOfFile(s->shmem);
        s->shmem = NULL;
    }
    if (s->shmem_mapping) {
        CloseHandle(s->shmem_mapping);
        s->shmem_mapping = NULL;
    }
}

#else /* POSIX */

static bool pvgpu_create_shmem_mapping(PvgpuState *s)
{
    /* Generate unique name for POSIX shared memory */
    snprintf(s->shmem_name, sizeof(s->shmem_name), "/pvgpu_shmem_%p", s);
    
    /* Create/open shared memory object */
    s->shmem_fd = shm_open(s->shmem_name, O_CREAT | O_RDWR, 0600);
    if (s->shmem_fd < 0) {
        error_report("pvgpu: failed to create shared memory");
        return false;
    }
    
    /* Set size */
    if (ftruncate(s->shmem_fd, s->shmem_size) < 0) {
        error_report("pvgpu: failed to set shared memory size");
        close(s->shmem_fd);
        shm_unlink(s->shmem_name);
        return false;
    }
    
    /* Map it */
    s->shmem = mmap(NULL, s->shmem_size, PROT_READ | PROT_WRITE, 
                    MAP_SHARED, s->shmem_fd, 0);
    if (s->shmem == MAP_FAILED) {
        error_report("pvgpu: failed to mmap shared memory");
        close(s->shmem_fd);
        shm_unlink(s->shmem_name);
        s->shmem = NULL;
        return false;
    }
    
    return true;
}

static void pvgpu_destroy_shmem_mapping(PvgpuState *s)
{
    if (s->shmem) {
        munmap(s->shmem, s->shmem_size);
        s->shmem = NULL;
    }
    if (s->shmem_fd >= 0) {
        close(s->shmem_fd);
        shm_unlink(s->shmem_name);
        s->shmem_fd = -1;
    }
}

#endif /* _WIN32 */

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
        pvgpu_notify_backend(s);
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
    
    /* Initialize handles */
#ifdef _WIN32
    s->backend_pipe_handle = INVALID_HANDLE_VALUE;
    s->shmem_mapping = NULL;
#else
    s->backend_socket = -1;
    s->shmem_fd = -1;
#endif
    s->backend_connected = false;
    s->backend_thread_running = false;
    
    /* Validate configuration */
    if (s->shmem_size < 64 * MiB) {
        error_setg(errp, "pvgpu: shmem_size must be at least 64MB");
        return;
    }
    
    /* Create shared memory file mapping */
    if (!pvgpu_create_shmem_mapping(s)) {
        error_setg(errp, "pvgpu: failed to create shared memory mapping");
        return;
    }
    
    /* Initialize BAR0: Control registers (4KB) */
    memory_region_init_io(&s->bar0, OBJECT(s), &pvgpu_bar0_ops, s,
                          "pvgpu-bar0", PVGPU_BAR0_SIZE);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    
    /* Initialize BAR2: Shared memory (already allocated via file mapping) */
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
    
    /* Mark device as ready (but no backend yet) */
    s->status = PVGPU_STATUS_READY;
    
    /* Try to connect to backend service */
    if (pvgpu_backend_connect(s)) {
        if (pvgpu_backend_handshake(s)) {
            s->backend_connected = true;
            s->status |= PVGPU_STATUS_BACKEND_CONN;
            
            /* Start backend message receiver thread */
            s->backend_thread_running = true;
            qemu_thread_create(&s->backend_thread, "pvgpu-backend",
                               pvgpu_backend_thread, s, QEMU_THREAD_JOINABLE);
        } else {
            pvgpu_backend_disconnect(s);
        }
    }
    
    if (!s->backend_connected) {
        /* Backend not available - device will work but no GPU acceleration */
        error_report("pvgpu: backend not connected - GPU acceleration unavailable");
    }
}

static void pvgpu_exit(PCIDevice *pci_dev)
{
    PvgpuState *s = PVGPU(pci_dev);
    
    /* Stop backend thread */
    if (s->backend_thread_running) {
        s->backend_thread_running = false;
        pvgpu_backend_disconnect(s);
        qemu_thread_join(&s->backend_thread);
    }
    
    if (s->msix_enabled) {
        msix_uninit_exclusive_bar(pci_dev);
    }
    
    pvgpu_destroy_shmem_mapping(s);
}

static void pvgpu_reset(DeviceState *dev)
{
    PvgpuState *s = PVGPU(dev);
    
    s->status = PVGPU_STATUS_READY;
    if (s->backend_connected) {
        s->status |= PVGPU_STATUS_BACKEND_CONN;
    }
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
