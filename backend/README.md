# PVGPU Backend Service

The PVGPU backend service runs on the Windows host and provides GPU acceleration for Windows guests. It connects to the QEMU pvgpu device, processes D3D11 commands from the guest, and renders frames using the host's real GPU.

## Quick Start

```powershell
# Build the backend
cd backend
cargo build --release

# Run with default settings
.\target\release\pvgpu-backend.exe

# Run with custom config
.\target\release\pvgpu-backend.exe --config pvgpu.toml
```

## Configuration

The backend can be configured via a TOML configuration file or command-line arguments.

### Configuration File

Create a file named `pvgpu.toml`:

```toml
# Named pipe path for QEMU device connection
# Default: \\.\pipe\pvgpu
pipe_path = "\\\\.\\pipe\\pvgpu"

# GPU adapter index (0 = default adapter)
# Use -1 or omit for auto-selection
# Use `dxdiag` or the backend's adapter enumeration to find index
adapter_index = 0

# Presentation mode: "headless", "windowed", "dual"
# - headless: No window, shared texture only (for streaming apps)
# - windowed: Opens a window to display rendered frames
# - dual: Both window and shared texture
presentation_mode = "headless"

# Initial display resolution
width = 1920
height = 1080

# VSync enabled (true/false)
# When true, limits frame rate to display refresh rate
vsync = true

# Number of frame buffers (2 = double buffering, 3 = triple buffering)
# Triple buffering reduces stuttering but increases latency
buffer_count = 2
```

### Configuration Options Reference

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `pipe_path` | string | `\\.\pipe\pvgpu` | Named pipe path for QEMU connection |
| `adapter_index` | u32 | 0 | GPU adapter index (0 = default) |
| `presentation_mode` | string | `headless` | Output mode (see below) |
| `width` | u32 | 1920 | Initial display width |
| `height` | u32 | 1080 | Initial display height |
| `vsync` | bool | true | Enable vertical sync |
| `buffer_count` | u32 | 2 | Frame buffer count (2 or 3) |

### Presentation Modes

#### `headless` (Default)
- No window displayed on the host
- Frames are rendered to a shared texture
- Ideal for streaming applications (Parsec, Sunshine, Moonlight)
- Lowest overhead

#### `windowed`
- Opens a Win32 window on the host desktop
- Displays rendered frames directly
- Useful for development and debugging
- Window can be resized (triggers swapchain recreation)

#### `dual`
- Both window display and shared texture
- Useful for debugging streaming setups
- Higher resource usage

### GPU Adapter Selection

To list available GPU adapters, run:

```powershell
# Using dxdiag
dxdiag /t dxdiag.txt
type dxdiag.txt | findstr "Card name"

# Or use the backend's enumeration (when implemented)
pvgpu-backend.exe --list-adapters
```

Common adapter indices:
- `0` - Primary/default GPU (usually integrated)
- `1` - Secondary GPU (usually discrete NVIDIA/AMD)

For multi-GPU systems, set the desired adapter explicitly:

```toml
# Use the NVIDIA GPU (typically index 1 on laptops)
adapter_index = 1
```

### VSync Configuration

| Setting | Use Case |
|---------|----------|
| `vsync = true` | Smooth gameplay, eliminates tearing |
| `vsync = false` | Lowest latency, may cause tearing |

For streaming with Parsec/Sunshine, `vsync = false` often provides better latency since the streaming encoder has its own frame pacing.

### Buffer Count

| Setting | Trade-off |
|---------|-----------|
| `buffer_count = 2` | Lower latency, possible micro-stuttering |
| `buffer_count = 3` | Smoother frames, ~1 frame additional latency |

## Environment Variables

The backend also respects these environment variables:

| Variable | Description |
|----------|-------------|
| `PVGPU_DEBUG` | Set to `1` for verbose debug logging |
| `PVGPU_CONFIG` | Path to configuration file |
| `RUST_LOG` | Standard Rust logging (e.g., `debug`, `trace`) |

Example:

```powershell
$env:RUST_LOG = "debug"
$env:PVGPU_DEBUG = "1"
.\pvgpu-backend.exe
```

## Logging

The backend uses the `tracing` crate for structured logging. Log levels:

- `error` - Critical failures
- `warn` - Recoverable issues
- `info` - Normal operation events
- `debug` - Detailed operation info
- `trace` - Very verbose (performance impact)

### Log Output Examples

**Startup:**
```
INFO  pvgpu_backend: PVGPU Backend Service starting...
INFO  pvgpu_backend: Protocol version: 1.0
INFO  pvgpu_backend: Configuration loaded: Config { pipe_path: "\\\\.\\pipe\\pvgpu", adapter_index: 0, ... }
INFO  pvgpu_backend: Initializing named pipe server...
```

**Connection:**
```
INFO  pvgpu_backend: Waiting for handshake from QEMU...
INFO  pvgpu_backend: Handshake received: shmem_name=Global\pvgpu_shmem_xxx, size=256MB
INFO  pvgpu_backend: Handshake complete!
```

**Operation:**
```
INFO  pvgpu_backend: Processing commands... (processed 1024 bytes)
DEBUG pvgpu_backend::command_processor: CMD_CREATE_RESOURCE: texture 1 (1920x1080 RGBA8)
DEBUG pvgpu_backend::command_processor: CMD_DRAW_INDEXED: 3600 indices, 6 instances
INFO  pvgpu_backend: Frame presented (fence 42)
```

### Log File Output

To redirect logs to a file:

```powershell
.\pvgpu-backend.exe 2>&1 | Tee-Object -FilePath pvgpu.log
```

Or use file appender in a future version (planned).

## Error Handling

The backend reports errors to the guest via the Control Region in shared memory:

| Error Code | Meaning |
|------------|---------|
| `0x0001` | Invalid command |
| `0x0002` | Invalid resource ID |
| `0x0003` | Resource not found |
| `0x0004` | Out of memory |
| `0x0005` | Shader compilation failed |
| `0x0006` | D3D11 device lost |
| `0x0007` | Backend internal error |

The guest driver checks these error codes and can respond accordingly (e.g., recreate lost resources, fallback rendering).

## Performance Tuning

### For Lowest Latency

```toml
presentation_mode = "headless"
vsync = false
buffer_count = 2
```

### For Smoothest Gameplay

```toml
presentation_mode = "windowed"  # or "headless" for streaming
vsync = true
buffer_count = 3
```

### For Streaming (Parsec/Sunshine)

```toml
presentation_mode = "headless"
vsync = false
buffer_count = 2
```

The streaming app will capture the shared texture directly.

## Troubleshooting

### Backend won't start

1. Check if another instance is running:
   ```powershell
   Get-Process pvgpu-backend -ErrorAction SilentlyContinue | Stop-Process
   ```

2. Verify named pipe isn't in use:
   ```powershell
   [System.IO.Directory]::GetFiles("\\.\pipe\") | Where-Object { $_ -like "*pvgpu*" }
   ```

### QEMU can't connect

1. Ensure backend is started BEFORE QEMU
2. Check pipe path matches between QEMU and backend config
3. Run QEMU with `-device pvgpu,backend_pipe=\\.\pipe\pvgpu`

### No frames displayed

1. Check backend logs for errors
2. Verify D3D11 device initialized successfully
3. Check guest driver is loaded and functioning
4. Verify presentation mode is `windowed` (not `headless`)

### High latency

1. Disable VSync: `vsync = false`
2. Reduce buffer count: `buffer_count = 2`
3. Check GPU isn't thermal throttling
4. Ensure correct GPU adapter is selected

### Device lost errors

If you see `PVGPU_ERROR_DEVICE_LOST`:
1. The host GPU may have reset (driver update, display change)
2. Backend will attempt to continue in degraded mode
3. Restart the VM for full recovery

## Building from Source

```powershell
cd backend

# Debug build
cargo build

# Release build (optimized)
cargo build --release

# Run tests
cargo test

# Check code formatting
cargo fmt -- --check

# Run linter
cargo clippy
```

### Dependencies

The backend uses these main Rust crates:
- `windows` - Windows API bindings (D3D11, DXGI)
- `tokio` - Async runtime
- `tracing` - Structured logging
- `anyhow` - Error handling
- `serde` / `toml` - Configuration parsing

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    PVGPU Backend                            │
│                                                             │
│  ┌─────────────┐    ┌──────────────────┐    ┌───────────┐  │
│  │ IPC Server  │───▶│ Command Processor │───▶│ D3D11     │  │
│  │ (pipe)      │    │                   │    │ Renderer  │  │
│  └─────────────┘    └──────────────────┘    └─────┬─────┘  │
│         │                    │                     │        │
│         │           ┌────────▼────────┐    ┌──────▼──────┐ │
│         │           │ Shared Memory   │    │Presentation │ │
│         │           │ (shmem module)  │    │  Pipeline   │ │
│         │           └─────────────────┘    └─────────────┘ │
└─────────┴──────────────────────────────────────────────────┘
          │                                         │
          │ Named Pipe                              │ Window/Texture
          ▼                                         ▼
     QEMU pvgpu device                    Host Display / Parsec
```

## License

MIT OR Apache-2.0
