# PVGPU Windows Drivers

This directory contains the Windows drivers for PVGPU:

- `kmd/` - Kernel-Mode Driver (WDDM 2.0 miniport)
- `umd/` - User-Mode Driver (D3D11 DDI DLL)

## Prerequisites

- **Windows Driver Kit (WDK)** - Download from [Microsoft](https://docs.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)
- **Visual Studio 2022** with Windows Driver development workload
- **Windows SDK** (matching WDK version)

## Building

### Using Visual Studio

1. Open `pvgpu-driver.sln` in Visual Studio 2022
2. Select configuration (Debug/Release) and platform (x64)
3. Build Solution

### Using MSBuild

```powershell
msbuild pvgpu-driver.sln /p:Configuration=Release /p:Platform=x64
```

## Test Signing

For development, you need to enable test signing in the guest VM:

```cmd
bcdedit /set testsigning on
```

Then reboot the guest.

## Installation

1. Copy the driver files to the guest VM
2. Open Device Manager
3. Right-click the "PCI Device" (PVGPU)
4. Update Driver → Browse my computer → Select the driver folder
5. Accept the test-signed driver warning

## Files

After building:

- `kmd/x64/Release/pvgpu.sys` - Kernel-mode driver
- `kmd/x64/Release/pvgpu.inf` - Driver installation file
- `umd/x64/Release/pvgpu_umd.dll` - User-mode D3D11 driver

## Development Status

⚠️ **Work in Progress** - These drivers are not yet implemented.

The current files are skeleton/placeholder code. Full implementation requires:

1. WDDM miniport interface implementation
2. D3D11 DDI implementation
3. Command ring producer logic
4. Fence synchronization

See the specs in `openspec/changes/windows-paravirt-gpu/specs/` for requirements.
