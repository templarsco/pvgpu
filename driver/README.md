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

## Test Signing Setup

**IMPORTANT:** PVGPU drivers are test-signed during development. You must enable test signing in the guest VM before installation.

### Step 1: Enable Test Signing Mode

Open an **elevated Command Prompt** (Run as Administrator) in the guest VM:

```cmd
bcdedit /set testsigning on
bcdedit /set nointegritychecks on
```

Reboot the guest. After reboot, you'll see a "Test Mode" watermark on the desktop - this is expected.

### Step 2: Disable Secure Boot (if enabled)

If your VM has Secure Boot enabled, you need to disable it:
1. Shut down the VM
2. In your VM configuration, disable Secure Boot
3. Start the VM

### Step 3: (Optional) Disable Driver Signature Enforcement Temporarily

For one-time testing without enabling test signing permanently:
1. Hold Shift while clicking Restart
2. Choose Troubleshoot → Advanced Options → Startup Settings → Restart
3. Press F7 to "Disable driver signature enforcement"
4. Install the driver (will need to repeat this after each reboot)

## Installation in Guest VM

### Prerequisites

Before installing, ensure:
1. QEMU is running with the pvgpu device (`-device pvgpu`)
2. Test signing is enabled (see above)
3. You have the driver files ready:
   - `pvgpu.sys` (kernel-mode driver)
   - `pvgpu.inf` (installation file)
   - `pvgpu_umd.dll` (64-bit user-mode driver)
   - `pvgpu_umd32.dll` (32-bit user-mode driver, optional)

### Method 1: Device Manager (Recommended for First-Time Install)

1. **Copy driver files** to the guest VM:
   - Copy the entire `bin/Release/x64/` folder to a location like `C:\Drivers\pvgpu\`

2. **Open Device Manager**:
   - Press Win+X → Device Manager
   - Or run `devmgmt.msc`

3. **Find the PVGPU device**:
   - Look under "Other devices" or "System devices"
   - Find "PCI Device" with yellow warning icon
   - Right-click → Properties → Details tab → Hardware Ids
   - Confirm it shows `PCI\VEN_1AF4&DEV_10F0`

4. **Install the driver**:
   - Right-click the device → "Update driver"
   - Select "Browse my computer for drivers"
   - Click "Browse" and select `C:\Drivers\pvgpu\`
   - Click "Next"
   - When prompted about test-signed driver, click "Install this driver software anyway"

5. **Verify installation**:
   - Device should now show as "PVGPU Paravirtualized GPU Adapter" under "Display adapters"

### Method 2: PnPUtil (Command Line)

Open an **elevated Command Prompt**:

```cmd
:: Navigate to driver folder
cd C:\Drivers\pvgpu

:: Install the driver to the driver store
pnputil /add-driver pvgpu.inf /install

:: If device is already present, scan for hardware changes
pnputil /scan-devices
```

### Method 3: DevCon

Download [DevCon](https://docs.microsoft.com/en-us/windows-hardware/drivers/devtest/devcon) from the WDK or SDK.

```cmd
:: Install for specific hardware ID
devcon install pvgpu.inf "PCI\VEN_1AF4&DEV_10F0"

:: Or update existing device
devcon update pvgpu.inf "PCI\VEN_1AF4&DEV_10F0"
```

### Method 4: Automated Script

Create a batch file `install_pvgpu.bat`:

```batch
@echo off
echo Installing PVGPU Driver...
echo.

:: Check for admin rights
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Please run as Administrator
    pause
    exit /b 1
)

:: Install driver
pnputil /add-driver "%~dp0pvgpu.inf" /install
if %errorlevel% neq 0 (
    echo ERROR: Driver installation failed
    pause
    exit /b 1
)

:: Copy UMD DLLs
copy /Y "%~dp0pvgpu_umd.dll" "%SystemRoot%\System32\"
if exist "%~dp0pvgpu_umd32.dll" (
    copy /Y "%~dp0pvgpu_umd32.dll" "%SystemRoot%\SysWOW64\"
)

echo.
echo Installation complete! Please restart the VM.
pause
```

## Verifying Installation

### Check Device Manager

1. Open Device Manager
2. Expand "Display adapters"
3. You should see "PVGPU Paravirtualized GPU Adapter"
4. Right-click → Properties → "This device is working properly"

### Check DxDiag

```cmd
dxdiag
```

In the "Display" tab, look for:
- **Name**: PVGPU Paravirtualized GPU Adapter
- **Manufacturer**: SANSI-GROUP
- **Chip Type**: PVGPU
- **DAC Type**: Unknown
- **Device Type**: Render-only device
- **Driver Version**: 1.0.0.0

### Check DirectX Feature Levels

```cmd
dxdiag /t dxdiag_output.txt
```

Review the output file for D3D11 feature level support.

### Verify UMD Registration

Check that the UMD DLL is correctly registered:

```cmd
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}" /s | findstr "pvgpu"
```

## Uninstalling the Driver

### Method 1: Device Manager

1. Open Device Manager
2. Right-click "PVGPU Paravirtualized GPU Adapter"
3. Select "Uninstall device"
4. Check "Delete the driver software for this device"
5. Click "Uninstall"

### Method 2: PnPUtil

```cmd
:: List installed drivers to find the published name
pnputil /enum-drivers | findstr "pvgpu"

:: Delete the driver (replace oem123.inf with actual name)
pnputil /delete-driver oem123.inf /uninstall /force
```

## Troubleshooting Installation

### "Windows cannot verify the digital signature"

**Cause**: Test signing not enabled
**Solution**: Enable test signing (see above) and reboot

### Device shows with yellow exclamation mark

**Possible causes**:
1. Backend service not running on host
2. Shared memory not initialized
3. Driver crash during initialization

**Solution**:
1. Check backend is running: `pvgpu-backend.exe`
2. Check Device Manager error code:
   - Code 10: General failure - check Event Viewer for details
   - Code 31: Driver load failure - check test signing
   - Code 43: Device reported a problem - check backend connection

### "PVGPU" not appearing in DxDiag

**Possible causes**:
1. UMD DLL not copied to System32
2. D3D11 not initializing properly

**Solution**:
1. Manually copy `pvgpu_umd.dll` to `C:\Windows\System32\`
2. Check Event Viewer → Windows Logs → System for errors
3. Restart the VM

### Performance Issues

1. Ensure VSync settings match between guest and backend
2. Check backend is using correct GPU (adapter_index in config)
3. Monitor command ring throughput in backend logs

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
