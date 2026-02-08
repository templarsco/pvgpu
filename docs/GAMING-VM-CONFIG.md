# PVGPU Gaming VM Configuration

This document provides sample QEMU command lines and configurations for running Windows gaming VMs with PVGPU.

## Prerequisites

1. **Windows 11 Host** with Hyper-V/WHPX enabled
2. **QEMU with WHPX support** (custom build with pvgpu device)
3. **PVGPU Backend** running on the host
4. **Windows 10/11 Guest** with PVGPU drivers installed

## Quick Start

### 1. Start the Backend

```powershell
# From pvgpu directory
.\backend\target\release\pvgpu-backend.exe
```

### 2. Start QEMU

```powershell
qemu-system-x86_64 ^
    -accel whpx ^
    -m 8G ^
    -cpu host ^
    -smp 4,sockets=1,cores=4,threads=1 ^
    -device pvgpu,shmem_size=256M ^
    -drive file=windows.qcow2,format=qcow2,if=virtio ^
    -nic user,model=virtio-net-pci ^
    -usb -device usb-tablet
```

## Sample Configurations

### Basic Gaming VM (Recommended)

A balanced configuration suitable for most games:

```powershell
# gaming-vm.bat
@echo off

set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
set VM_DISK=C:\VMs\windows-gaming.qcow2
set MEMORY=8G
set CORES=4

"%QEMU%" ^
    -name "Gaming VM" ^
    -accel whpx ^
    -m %MEMORY% ^
    -cpu host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time ^
    -smp %CORES%,sockets=1,cores=%CORES%,threads=1 ^
    -device pvgpu,shmem_size=256M,backend_pipe=\\.\pipe\pvgpu ^
    -drive file=%VM_DISK%,format=qcow2,if=virtio,cache=writeback ^
    -nic user,model=virtio-net-pci,hostfwd=tcp::3389-:3389 ^
    -usb -device usb-tablet ^
    -rtc base=localtime ^
    -boot order=c
```

### High-Performance Gaming VM

For demanding games with more resources:

```powershell
# high-perf-vm.bat
@echo off

set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
set VM_DISK=C:\VMs\windows-gaming.qcow2

"%QEMU%" ^
    -name "High-Performance Gaming VM" ^
    -accel whpx ^
    -m 16G ^
    -cpu host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time,hv_stimer,hv_tlbflush ^
    -smp 8,sockets=1,cores=8,threads=1 ^
    -device pvgpu,shmem_size=512M,backend_pipe=\\.\pipe\pvgpu ^
    -drive file=%VM_DISK%,format=qcow2,if=virtio,cache=none,aio=native ^
    -nic user,model=virtio-net-pci ^
    -usb -device usb-tablet ^
    -rtc base=localtime ^
    -global kvm-pit.lost_tick_policy=delay ^
    -boot order=c
```

### Streaming-Ready VM (Parsec/Sunshine)

Optimized for remote gaming with streaming:

```powershell
# streaming-vm.bat
@echo off

set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
set VM_DISK=C:\VMs\windows-streaming.qcow2

"%QEMU%" ^
    -name "Streaming VM" ^
    -accel whpx ^
    -m 12G ^
    -cpu host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time ^
    -smp 6,sockets=1,cores=6,threads=1 ^
    -device pvgpu,shmem_size=512M,backend_pipe=\\.\pipe\pvgpu ^
    -drive file=%VM_DISK%,format=qcow2,if=virtio,cache=writeback ^
    -nic user,model=virtio-net-pci,hostfwd=tcp::3389-:3389,hostfwd=udp::9000-:9000,hostfwd=tcp::9000-:9000 ^
    -usb -device usb-tablet ^
    -rtc base=localtime ^
    -boot order=c
```

Backend configuration for streaming (`pvgpu-streaming.toml`):

```toml
pipe_path = "\\\\.\\pipe\\pvgpu"
adapter_index = 0
presentation_mode = "headless"
width = 1920
height = 1080
vsync = false
buffer_count = 2
```

### Anti-Detection VM (Custom Identity)

For testing with custom VM identity:

```powershell
# stealth-vm.bat
@echo off

set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
set VM_DISK=C:\VMs\windows-stealth.qcow2

"%QEMU%" ^
    -name "Custom PC" ^
    -accel whpx ^
    -m 8G ^
    -cpu host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time ^
    -smp 4,sockets=1,cores=4,threads=1 ^
    -device pvgpu,shmem_size=256M ^
    -drive file=%VM_DISK%,format=qcow2,if=virtio ^
    -nic user,model=virtio-net-pci ^
    -usb -device usb-tablet ^
    -smbios type=0,vendor="American Megatrends Inc.",version="F8" ^
    -smbios type=1,manufacturer="ASUS",product="ROG STRIX B550-F",version="Rev 1.0",serial="SN123456789",uuid=auto ^
    -smbios type=2,manufacturer="ASUS",product="ROG STRIX B550-F GAMING" ^
    -smbios type=3,manufacturer="Default",type=3,version="1.0" ^
    -rtc base=localtime ^
    -boot order=c
```

## PVGPU Device Options

| Option | Default | Description |
|--------|---------|-------------|
| `shmem_size` | `256M` | Shared memory size (128M-1G) |
| `backend_pipe` | `\\.\pipe\pvgpu` | Named pipe path for backend connection |

### Recommended Shared Memory Sizes

| Resolution | Min Size | Recommended |
|------------|----------|-------------|
| 720p | 128M | 128M |
| 1080p | 128M | 256M |
| 1440p | 256M | 384M |
| 4K | 384M | 512M |

Larger sizes allow more concurrent resources and textures in the resource heap.

## CPU Configuration

### Hyper-V Enlightenments

For best performance on WHPX, enable these Hyper-V features:

```
-cpu host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time,hv_stimer,hv_tlbflush
```

| Feature | Description |
|---------|-------------|
| `hv_relaxed` | Relaxed timing - reduces timer overhead |
| `hv_spinlocks` | Paravirtualized spinlocks |
| `hv_vapic` | Virtual APIC |
| `hv_time` | Reference time counter |
| `hv_stimer` | Synthetic timers |
| `hv_tlbflush` | TLB flush optimization |

### Core/Thread Configuration

```
-smp <total>,sockets=1,cores=<n>,threads=<t>
```

- **Gaming**: Use physical cores only (threads=1) for best latency
- **Multi-tasking**: Enable SMT (threads=2) for more virtual CPUs
- Leave 2+ cores for the host OS

## Memory Configuration

```
-m <size>
```

Recommended:
- **Minimum**: 4G
- **Gaming**: 8G-12G
- **AAA Games**: 16G

Consider host memory and leave ~4GB for the host OS.

## Storage Configuration

### Virtio (Recommended)

```
-drive file=disk.qcow2,format=qcow2,if=virtio,cache=writeback
```

Requires virtio-win drivers in guest.

### IDE (Compatibility)

```
-drive file=disk.qcow2,format=qcow2,if=ide
```

No additional drivers needed but slower.

### Raw Image (Best Performance)

```
-drive file=disk.raw,format=raw,if=virtio,cache=none,aio=native
```

Best I/O performance but no snapshots.

## Network Configuration

### User Mode (NAT)

```
-nic user,model=virtio-net-pci,hostfwd=tcp::3389-:3389
```

Simple but limited. Port forwarding for RDP/streaming.

### Bridged Network

```
-nic tap,model=virtio-net-pci,ifname=tap0
```

Full network access, requires TAP adapter setup.

## Input Devices

### USB Tablet (Absolute Positioning)

```
-usb -device usb-tablet
```

Best for GUI interaction - no mouse capture.

### USB Mouse/Keyboard

```
-usb -device usb-kbd -device usb-mouse
```

Relative mouse - may require capture.

## Display Output

### VNC (Remote Access)

```
-vnc :0 -device VGA
```

Access via `localhost:5900` from any VNC client.

### SDL Window

```
-display sdl
```

Opens a window on the host (not needed if using PVGPU window).

### No Display

```
-display none
```

Headless mode - use RDP or streaming to access.

## Full Example: Gaming-Ready VM

Complete script with all recommended settings:

```powershell
# start-gaming-vm.ps1
param(
    [string]$VMDisk = "C:\VMs\windows-gaming.qcow2",
    [int]$Memory = 8,
    [int]$Cores = 4
)

# Start backend first
$backendJob = Start-Job -ScriptBlock {
    & "C:\pvgpu\backend\target\release\pvgpu-backend.exe"
}

Start-Sleep -Seconds 2  # Wait for backend to initialize

# Start QEMU
$qemuArgs = @(
    "-name", "Gaming VM",
    "-accel", "whpx",
    "-m", "${Memory}G",
    "-cpu", "host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time",
    "-smp", "$Cores,sockets=1,cores=$Cores,threads=1",
    "-device", "pvgpu,shmem_size=256M",
    "-drive", "file=$VMDisk,format=qcow2,if=virtio,cache=writeback",
    "-nic", "user,model=virtio-net-pci,hostfwd=tcp::3389-:3389",
    "-usb", "-device", "usb-tablet",
    "-rtc", "base=localtime",
    "-boot", "order=c"
)

& "C:\Program Files\qemu\qemu-system-x86_64.exe" $qemuArgs

# Cleanup
Stop-Job $backendJob
Remove-Job $backendJob
```

## Troubleshooting

### QEMU won't start with WHPX

1. Enable Hyper-V in Windows Features
2. Run PowerShell as Administrator:
   ```powershell
   Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All
   ```
3. Reboot

### PVGPU device not found

1. Ensure using custom QEMU build with pvgpu
2. Check device is registered: `qemu-system-x86_64 -device help | findstr pvgpu`

### Poor performance

1. Enable Hyper-V enlightenments
2. Use virtio drivers in guest
3. Check backend is using correct GPU adapter
4. Disable VSync if latency matters more than smoothness

### Guest freezes during gaming

1. Increase shared memory size: `shmem_size=512M`
2. Check for command ring overflow in backend logs
3. Ensure sufficient host memory

## See Also

- [QEMU Documentation](https://www.qemu.org/docs/master/)
- [Backend Configuration](../backend/README.md)
- [Driver Installation](../driver/README.md)
