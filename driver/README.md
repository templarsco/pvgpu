# PVGPU Windows Drivers

This directory contains the Windows WDDM drivers for PVGPU:

- `kmd/` - Kernel-Mode Driver (WDDM 2.0 miniport)
- `umd/` - User-Mode Driver (D3D11 DDI DLL)
- `pvgpu.sln` - Visual Studio solution for both drivers

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Windows Guest                                              │
│                                                             │
│  ┌─────────────┐     ┌──────────────────────────────────┐   │
│  │   Game/App  │────▶│  D3D11 Runtime (d3d11.dll)       │   │
│  └─────────────┘     └──────────────────────────────────┘   │
│                                   │                         │
│                                   ▼                         │
│                      ┌──────────────────────────────────┐   │
│                      │  pvgpu_umd.dll (User-Mode Driver)│   │
│                      │  - D3D11 DDI implementation      │   │
│                      │  - Command buffer building       │   │
│                      └──────────────────────────────────┘   │
│                                   │                         │
│                                   ▼                         │
│                      ┌──────────────────────────────────┐   │
│                      │  pvgpu.sys (Kernel-Mode Driver)  │   │
│                      │  - WDDM miniport                 │   │
│                      │  - BAR mapping & DMA             │   │
│                      │  - Command ring submission       │   │
│                      └──────────────────────────────────┘   │
│                                   │                         │
├───────────────────────────────────┼─────────────────────────┤
│  QEMU Virtual Device              │                         │
│                      ┌────────────▼─────────────────────┐   │
│                      │  pvgpu PCIe Device               │   │
│                      │  - BAR0: Config registers        │   │
│                      │  - BAR2: Shared memory           │   │
│                      └──────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## Prerequisites

- **Visual Studio 2022** with "Desktop development with C++" workload
- **Windows Driver Kit (WDK)** - Download from [Microsoft](https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)
- **Windows SDK** (matching WDK version)

### Installing WDK

1. Install Visual Studio 2022 first
2. Download WDK from Microsoft
3. Run the WDK installer
4. Install the Visual Studio extension when prompted

## Building

### Using Visual Studio

1. Open `driver/pvgpu.sln` in Visual Studio 2022
2. Select configuration (Debug/Release) and platform (x64)
3. Build → Build Solution (Ctrl+Shift+B)

### Using MSBuild (Command Line)

Open "Developer Command Prompt for VS 2022" and run:

```powershell
cd driver
msbuild pvgpu.sln /p:Configuration=Release /p:Platform=x64
```

### Build Outputs

After building, files are placed in:

```
driver/
├── bin/
│   └── Release/
│       └── x64/
│           ├── pvgpu.sys          # Kernel-mode driver
│           ├── pvgpu.inf          # Driver INF
│           ├── pvgpu.cat          # Catalog (after signing)
│           ├── pvgpu_umd.dll      # 64-bit user-mode driver
│           └── pvgpu_umd32.dll    # 32-bit user-mode driver (if built)
```

## Test Signing

For development, you need to enable test signing in the guest VM:

```cmd
bcdedit /set testsigning on
```

Then reboot the guest. You'll see "Test Mode" watermark on the desktop.

## Installation in Guest VM

### Method 1: Device Manager

1. Copy the entire `bin/Release/x64/` folder to the guest VM
2. Open Device Manager
3. Find the PVGPU device (shows as "PCI Device" with unknown driver)
   - Vendor ID: 1AF4, Device ID: 10F0
4. Right-click → Update Driver
5. Browse my computer for drivers → Select the copied folder
6. Accept the test-signed driver warning

### Method 2: PnPUtil (Command Line)

```cmd
pnputil /add-driver pvgpu.inf /install
```

### Method 3: DevCon

```cmd
devcon install pvgpu.inf "PCI\VEN_1AF4&DEV_10F0"
```

## Verifying Installation

After installation, you should see:
- "PVGPU Paravirtualized GPU Adapter" in Device Manager under Display adapters
- The UMD DLLs copied to `C:\Windows\System32\` (and SysWOW64 for 32-bit)

Check driver status:
```cmd
dxdiag
```

Look for "PVGPU" in the Display tab.

## Driver Components

### Kernel-Mode Driver (kmd/)

| File | Description |
|------|-------------|
| `pvgpu.c` | Main driver implementation (~650 lines) |
| `pvgpu.h` | Device context and callback prototypes |
| `pvgpu.inf` | Driver installation file |
| `pvgpu.vcxproj` | Visual Studio project |

Key functionality:
- WDDM miniport callbacks (DxgkDdiXxx)
- BAR0/BAR2 mapping
- MSI-X interrupt handling
- Command ring submission to host

### User-Mode Driver (umd/)

| File | Description |
|------|-------------|
| `pvgpu_umd.c` | D3D11 DDI implementation (~900 lines) |
| `pvgpu_umd.h` | Structures and prototypes |
| `pvgpu_umd.def` | DLL exports |
| `pvgpu_umd.vcxproj` | Visual Studio project |

Key functionality:
- `OpenAdapter10_2` entry point
- Resource/shader creation and destruction
- Draw command translation
- Command buffer management
- Present handling

## Development Status

| Component | Status | Notes |
|-----------|--------|-------|
| KMD skeleton | ✅ Complete | Basic structure with all WDDM callbacks |
| UMD skeleton | ✅ Complete | D3D11 DDI with draw commands |
| BAR mapping | ✅ Implemented | In KMD StartDevice |
| Interrupt handling | ⚠️ Basic | ISR/DPC stubs |
| Resource allocation | ⚠️ Stub | Needs shared memory integration |
| Draw commands | ✅ Implemented | In UMD |
| Pipeline state | ⚠️ Stubs | Needs full implementation |
| Resource mapping | ❌ TODO | For UpdateSubresource, Map/Unmap |
| Present | ⚠️ Basic | Needs proper swapchain handling |

## Debugging

### Enable Debug Output

For KMD (requires kernel debugger):
```
kd> ed nt!Kd_IHVVIDEO_Mask 0xFFFFFFFF
```

For UMD (in guest):
```cmd
set PVGPU_DEBUG=1
```

Use DebugView or OutputDebugString viewer to see UMD traces.

### Common Issues

1. **Driver won't load**: Check test signing is enabled
2. **Device not detected**: Ensure QEMU is started with pvgpu device
3. **Black screen**: Backend service may not be running on host
4. **Performance issues**: Check command ring isn't stalling

## Related Documentation

- `protocol/pvgpu_protocol.h` - Command protocol definition
- `backend/` - Host-side D3D11 renderer
- `qemu-device/` - QEMU virtual device

## License

MIT OR Apache-2.0
