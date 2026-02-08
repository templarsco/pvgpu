# PVGPU QEMU Device

This directory contains the QEMU device implementation for PVGPU (Paravirtualized GPU).

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Windows Guest VM                         │
│  ┌──────────┐    ┌──────────┐    ┌─────────────────────────┐  │
│  │ Game/App │───►│ UMD DLL  │───►│    Shared Memory (BAR2) │  │
│  └──────────┘    └──────────┘    │  - Control Region       │  │
│                        │         │  - Command Ring         │  │
│                        │         │  - Resource Heap        │  │
│                  ┌─────┴─────┐   └───────────┬─────────────┘  │
│                  │    KMD    │               │                 │
│                  └───────────┘               │                 │
└──────────────────────────────────────────────┼─────────────────┘
                                               │
┌──────────────────────────────────────────────┼─────────────────┐
│                     QEMU Host                │                 │
│  ┌────────────────┐                          │                 │
│  │  pvgpu device  │◄─────────────────────────┘                 │
│  │   (this code)  │                                            │
│  └───────┬────────┘                                            │
│          │ Named Pipe                                          │
│          │ (handshake, doorbell, IRQ)                          │
│  ┌───────▼────────┐    ┌─────────────────┐                    │
│  │ pvgpu-backend  │───►│  D3D11 Renderer │───► Display/Stream │
│  │    (Rust)      │    └─────────────────┘                    │
│  └────────────────┘                                            │
└────────────────────────────────────────────────────────────────┘
```

## Files

- `pvgpu.c` - Main device implementation
- `pvgpu.h` - Internal header
- `meson.build` - Meson build file (reference)
- `meson_options.txt` - Build options

## Integration with QEMU

### Step 1: Copy files to QEMU source tree

```bash
# From pvgpu repository root
QEMU_SRC=/path/to/qemu

cp qemu-device/pvgpu.c $QEMU_SRC/hw/display/
cp qemu-device/pvgpu.h $QEMU_SRC/hw/display/
cp protocol/pvgpu_protocol.h $QEMU_SRC/include/hw/display/
```

### Step 2: Add to QEMU Meson build

Edit `$QEMU_SRC/hw/display/meson.build` and add:

```meson
softmmu_ss.add(when: 'CONFIG_PVGPU', if_true: files('pvgpu.c'))
```

### Step 3: Add Kconfig option

Edit `$QEMU_SRC/hw/display/Kconfig` and add:

```
config PVGPU
    bool
    default y if PCI_DEVICES
    depends on PCI
    help
      Paravirtualized GPU device for Windows guests.
      Requires pvgpu-backend service running on host.
```

### Step 4: Update protocol header include path

In the copied `pvgpu.c`, change the include from:
```c
#include "../../protocol/pvgpu_protocol.h"
```
to:
```c
#include "hw/display/pvgpu_protocol.h"
```

### Step 5: Build QEMU

```bash
cd $QEMU_SRC
mkdir build && cd build

# For Windows with WHPX
../configure --target-list=x86_64-softmmu --enable-whpx

# Build
make -j$(nproc)
```

## Device Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `shmem_size` | uint32 | 268435456 (256MB) | Shared memory size in bytes |
| `backend_pipe` | string | `\\.\pipe\pvgpu` (Win) / `/tmp/pvgpu.sock` (Linux) | Backend IPC path |

## Usage Example

```bash
# Start the backend first
./pvgpu-backend.exe

# Then start QEMU
qemu-system-x86_64 \
    -accel whpx \
    -m 8G \
    -cpu host \
    -device pvgpu,shmem_size=256M \
    -drive file=windows.qcow2,if=virtio \
    ...
```

## IPC Protocol

The device communicates with the backend via named pipe (Windows) or Unix socket (Linux):

### Message Format
```
┌──────────────┬──────────────┬─────────────────┐
│ msg_type(u32)│payload_size  │    payload      │
│              │    (u32)     │  (variable)     │
└──────────────┴──────────────┴─────────────────┘
```

### Message Types
| Type | Direction | Payload | Description |
|------|-----------|---------|-------------|
| 1 | QEMU→Backend | shmem_size(u64) + shmem_name(str) | Handshake |
| 2 | Backend→QEMU | features(u64) | Handshake acknowledgement |
| 3 | QEMU→Backend | (none) | Doorbell notification |
| 4 | Backend→QEMU | vector(u32) | IRQ request |
| 5 | Both | (none) | Shutdown |

## PCI Configuration

| Field | Value | Description |
|-------|-------|-------------|
| Vendor ID | 0x1AF4 | Red Hat (virtio vendor) |
| Device ID | 0x10F0 | PVGPU device |
| Class | 0x030000 | VGA-compatible display controller |
| Revision | 0x01 | Protocol version |

## BARs

| BAR | Size | Type | Description |
|-----|------|------|-------------|
| BAR0 | 4KB | MMIO | Control registers |
| BAR1 | 4KB | MMIO | MSI-X table (if supported) |
| BAR2 | 256MB+ | Memory | Shared memory (prefetchable) |

## Notes

- The LSP errors when viewing this code outside QEMU are expected (missing QEMU headers)
- On Windows, the shared memory is created as a named file mapping (`Global\pvgpu_shmem_<ptr>`)
- On Linux, POSIX shared memory is used (`/pvgpu_shmem_<ptr>`)
- The backend must be running before QEMU starts for GPU acceleration
- If backend is unavailable, the device initializes but reports no backend connection
