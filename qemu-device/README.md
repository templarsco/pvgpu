# PVGPU QEMU Device

This directory contains the QEMU device implementation for PVGPU.

## Build Instructions

This code is designed to be built as part of the QEMU source tree. To integrate:

1. **Copy files to QEMU source:**
   ```bash
   cp pvgpu.c pvgpu.h /path/to/qemu/hw/display/
   cp ../protocol/pvgpu_protocol.h /path/to/qemu/hw/display/
   ```

2. **Add to QEMU build system (hw/display/meson.build):**
   ```meson
   softmmu_ss.add(when: 'CONFIG_PVGPU', if_true: files('pvgpu.c'))
   ```

3. **Add config option (hw/display/Kconfig):**
   ```
   config PVGPU
       bool
       default y if PCI_DEVICES
       depends on PCI
   ```

4. **Build QEMU:**
   ```bash
   cd /path/to/qemu
   mkdir build && cd build
   ../configure --target-list=x86_64-softmmu --enable-whpx
   make
   ```

## Usage

```bash
qemu-system-x86_64 \
    -accel whpx \
    -device pvgpu,shmem_size=256M,backend_pipe=\\.\pipe\pvgpu \
    ...
```

## Notes

- The LSP errors in pvgpu.c/pvgpu.h are expected when viewing outside the QEMU source tree
- Headers like `qemu/osdep.h` are provided by QEMU
- This device requires Windows with WHPX for optimal performance
- The backend service (pvgpu-backend) must be running on the host
