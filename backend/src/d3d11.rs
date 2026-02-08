//! D3D11 Renderer Module
//!
//! Handles D3D11 device creation, resource management, and command execution.
//! This module wraps Direct3D 11 APIs to execute graphics commands received
//! from the guest via the command ring.

use anyhow::{anyhow, Result};
use tracing::{debug, info, warn};
use windows::core::Interface;
use windows::Win32::Graphics::Direct3D::{
    D3D_DRIVER_TYPE_UNKNOWN, D3D_FEATURE_LEVEL, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
    D3D_PRIMITIVE_TOPOLOGY,
};
use windows::Win32::Graphics::Direct3D11::{
    D3D11CreateDevice, ID3D11BlendState, ID3D11Buffer, ID3D11ComputeShader,
    ID3D11DepthStencilState, ID3D11DepthStencilView, ID3D11Device, ID3D11DeviceContext,
    ID3D11DomainShader, ID3D11GeometryShader, ID3D11HullShader, ID3D11InputLayout,
    ID3D11PixelShader, ID3D11RasterizerState, ID3D11RenderTargetView, ID3D11Resource,
    ID3D11SamplerState, ID3D11ShaderResourceView, ID3D11Texture2D, ID3D11VertexShader,
    D3D11_BIND_RENDER_TARGET, D3D11_BIND_SHADER_RESOURCE, D3D11_BUFFER_DESC,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_SDK_VERSION, D3D11_SUBRESOURCE_DATA,
    D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT, D3D11_VIEWPORT,
};
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT, DXGI_SAMPLE_DESC};
use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIAdapter1, IDXGIFactory1};

/// Resource ID type (matches guest resource IDs)
pub type ResourceId = u32;

/// D3D11 resource wrapper - holds the actual D3D11 objects
#[allow(dead_code)]
pub enum D3D11Resource {
    Texture2D {
        texture: ID3D11Texture2D,
        width: u32,
        height: u32,
        format: DXGI_FORMAT,
        srv: Option<ID3D11ShaderResourceView>,
        rtv: Option<ID3D11RenderTargetView>,
    },
    Buffer {
        buffer: ID3D11Buffer,
        size: u32,
        bind_flags: u32,
    },
    VertexShader {
        shader: ID3D11VertexShader,
        bytecode: Vec<u8>,
    },
    PixelShader {
        shader: ID3D11PixelShader,
    },
    GeometryShader {
        shader: ID3D11GeometryShader,
    },
    HullShader {
        shader: ID3D11HullShader,
    },
    DomainShader {
        shader: ID3D11DomainShader,
    },
    ComputeShader {
        shader: ID3D11ComputeShader,
    },
    InputLayout {
        layout: ID3D11InputLayout,
    },
    BlendState {
        state: ID3D11BlendState,
    },
    RasterizerState {
        state: ID3D11RasterizerState,
    },
    DepthStencilState {
        state: ID3D11DepthStencilState,
    },
    SamplerState {
        state: ID3D11SamplerState,
    },
    RenderTargetView {
        rtv: ID3D11RenderTargetView,
    },
    DepthStencilView {
        dsv: ID3D11DepthStencilView,
    },
    ShaderResourceView {
        srv: ID3D11ShaderResourceView,
    },
}

/// Adapter information
#[derive(Debug, Clone)]
pub struct AdapterInfo {
    pub index: u32,
    pub description: String,
    pub vendor_id: u32,
    pub device_id: u32,
    pub dedicated_video_memory: usize,
    pub luid: u64,
}

/// Holds all D3D11 resources and state
#[allow(dead_code)]
pub struct D3D11Renderer {
    /// D3D11 device
    device: ID3D11Device,
    /// Immediate context for command execution
    context: ID3D11DeviceContext,
    /// Feature level achieved
    feature_level: D3D_FEATURE_LEVEL,
    /// DXGI factory for adapter enumeration
    factory: IDXGIFactory1,
    /// Selected adapter info
    adapter_info: AdapterInfo,
    /// Resource slab: guest resource ID → D3D11 resource.
    /// Uses Vec<Option<>> indexed by resource ID for O(1) lookup.
    /// Resource IDs are sequential from 1, making this far faster than HashMap.
    resources: Vec<Option<D3D11Resource>>,
    /// Current render targets
    current_rtvs: Vec<Option<ID3D11RenderTargetView>>,
    /// Current depth stencil view
    current_dsv: Option<ID3D11DepthStencilView>,
}

impl D3D11Renderer {
    /// Enumerate available GPU adapters
    pub fn enumerate_adapters() -> Result<Vec<AdapterInfo>> {
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1()? };
        let mut adapters = Vec::new();

        for i in 0.. {
            let adapter: Result<IDXGIAdapter1, _> = unsafe { factory.EnumAdapters1(i) };

            match adapter {
                Ok(adapter) => {
                    let desc = unsafe { adapter.GetDesc1()? };

                    let description = String::from_utf16_lossy(
                        &desc.Description[..desc
                            .Description
                            .iter()
                            .position(|&c| c == 0)
                            .unwrap_or(desc.Description.len())],
                    );

                    let luid = ((desc.AdapterLuid.HighPart as u64) << 32)
                        | (desc.AdapterLuid.LowPart as u64);

                    adapters.push(AdapterInfo {
                        index: i,
                        description,
                        vendor_id: desc.VendorId,
                        device_id: desc.DeviceId,
                        dedicated_video_memory: desc.DedicatedVideoMemory,
                        luid,
                    });
                }
                Err(_) => break,
            }
        }

        Ok(adapters)
    }

    /// Create a new D3D11 renderer with the specified adapter
    pub fn new(adapter_index: Option<u32>) -> Result<Self> {
        info!("Creating D3D11 device...");

        // Create DXGI factory
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1()? };

        // Get adapter and info
        let index = adapter_index.unwrap_or(0);
        let adapter: IDXGIAdapter1 = unsafe { factory.EnumAdapters1(index)? };
        let desc = unsafe { adapter.GetDesc1()? };

        let description = String::from_utf16_lossy(
            &desc.Description[..desc
                .Description
                .iter()
                .position(|&c| c == 0)
                .unwrap_or(desc.Description.len())],
        );

        let luid = ((desc.AdapterLuid.HighPart as u64) << 32) | (desc.AdapterLuid.LowPart as u64);

        let adapter_info = AdapterInfo {
            index,
            description,
            vendor_id: desc.VendorId,
            device_id: desc.DeviceId,
            dedicated_video_memory: desc.DedicatedVideoMemory,
            luid,
        };

        info!(
            "Using GPU: {} (VRAM: {} MB)",
            adapter_info.description,
            adapter_info.dedicated_video_memory / (1024 * 1024)
        );

        // Feature levels to try
        let feature_levels = [D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0];

        // Create flags
        let flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        #[cfg(debug_assertions)]
        let flags = {
            use windows::Win32::Graphics::Direct3D11::D3D11_CREATE_DEVICE_DEBUG;
            flags | D3D11_CREATE_DEVICE_DEBUG
        };

        // Create device
        let mut device: Option<ID3D11Device> = None;
        let mut context: Option<ID3D11DeviceContext> = None;
        let mut achieved_level = D3D_FEATURE_LEVEL_11_0;

        unsafe {
            D3D11CreateDevice(
                &adapter,
                D3D_DRIVER_TYPE_UNKNOWN,
                None,
                flags,
                Some(&feature_levels),
                D3D11_SDK_VERSION,
                Some(&mut device),
                Some(&mut achieved_level),
                Some(&mut context),
            )?;
        }

        let device = device.ok_or_else(|| anyhow!("Failed to create D3D11 device"))?;
        let context = context.ok_or_else(|| anyhow!("Failed to get device context"))?;

        info!(
            "D3D11 device created with feature level: {:?}",
            achieved_level
        );

        Ok(Self {
            device,
            context,
            feature_level: achieved_level,
            factory,
            adapter_info,
            resources: Vec::with_capacity(1024),
            current_rtvs: vec![None; 8],
            current_dsv: None,
        })
    }

    // -- Resource slab helpers --
    // Resource IDs from the guest start at 1 and are sequential.
    // We use the ID as a direct index into a Vec<Option<D3D11Resource>>
    // for O(1) lookup instead of HashMap's hash+probe overhead.

    /// Insert a resource into the slab at the given ID.
    fn slab_insert(&mut self, id: ResourceId, resource: D3D11Resource) {
        let idx = id as usize;
        if idx >= self.resources.len() {
            self.resources.resize_with(idx + 1, || None);
        }
        self.resources[idx] = Some(resource);
    }

    /// Get a reference to a resource by ID.
    fn slab_get(&self, id: ResourceId) -> Option<&D3D11Resource> {
        self.resources.get(id as usize).and_then(|r| r.as_ref())
    }

    /// Remove a resource by ID, returning it if present.
    fn slab_remove(&mut self, id: ResourceId) -> Option<D3D11Resource> {
        let idx = id as usize;
        if idx < self.resources.len() {
            self.resources[idx].take()
        } else {
            None
        }
    }

    /// Get the count of active (non-None) resources.
    fn slab_count(&self) -> usize {
        self.resources.iter().filter(|r| r.is_some()).count()
    }

    /// Clear all resources from the slab.
    fn slab_clear(&mut self) {
        self.resources.clear();
    }

    /// Get device reference
    pub fn device(&self) -> &ID3D11Device {
        &self.device
    }

    /// Get context reference
    pub fn context(&self) -> &ID3D11DeviceContext {
        &self.context
    }

    /// Get adapter info
    pub fn adapter_info(&self) -> &AdapterInfo {
        &self.adapter_info
    }

    /// Check if the device is in a lost/removed state.
    /// Returns true if the device is still valid, false if lost.
    pub fn check_device_status(&self) -> bool {
        use windows::Win32::Graphics::Dxgi::{
            DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET,
            DXGI_ERROR_DRIVER_INTERNAL_ERROR, DXGI_ERROR_INVALID_CALL,
        };

        // Get the DXGI device and check GetDeviceRemovedReason
        let dxgi_device: Result<windows::Win32::Graphics::Dxgi::IDXGIDevice, _> =
            self.device.cast();

        if let Ok(dxgi_device) = dxgi_device {
            // Try to get device parent (adapter) - this will fail if device is lost
            let adapter_result: Result<IDXGIAdapter1, _> = unsafe { dxgi_device.GetParent() };
            if adapter_result.is_err() {
                warn!("Device lost: failed to get adapter");
                return false;
            }
        }

        // Check GetDeviceRemovedReason on the D3D11 device
        let reason = unsafe { self.device.GetDeviceRemovedReason() };

        // S_OK (0) means device is fine
        if reason.is_ok() {
            return true;
        }

        // Log the specific reason
        let hr = reason.unwrap_err().code().0 as u32;
        match hr {
            x if x == DXGI_ERROR_DEVICE_REMOVED.0 as u32 => {
                warn!("Device lost: DXGI_ERROR_DEVICE_REMOVED");
            }
            x if x == DXGI_ERROR_DEVICE_HUNG.0 as u32 => {
                warn!("Device lost: DXGI_ERROR_DEVICE_HUNG");
            }
            x if x == DXGI_ERROR_DEVICE_RESET.0 as u32 => {
                warn!("Device lost: DXGI_ERROR_DEVICE_RESET");
            }
            x if x == DXGI_ERROR_DRIVER_INTERNAL_ERROR.0 as u32 => {
                warn!("Device lost: DXGI_ERROR_DRIVER_INTERNAL_ERROR");
            }
            x if x == DXGI_ERROR_INVALID_CALL.0 as u32 => {
                warn!("Device lost: DXGI_ERROR_INVALID_CALL");
            }
            _ => {
                warn!("Device lost: unknown reason 0x{:08X}", hr);
            }
        }

        false
    }

    /// Get the number of resources currently tracked
    pub fn resource_count(&self) -> usize {
        self.slab_count()
    }

    /// Clear all resources (useful before device recreation)
    pub fn clear_resources(&mut self) {
        info!("Clearing {} resources", self.slab_count());
        self.slab_clear();
        self.current_rtvs = vec![None; 8];
        self.current_dsv = None;
    }

    /// Create a 2D texture
    pub fn create_texture2d(
        &mut self,
        id: ResourceId,
        width: u32,
        height: u32,
        format: DXGI_FORMAT,
        bind_flags: u32,
        initial_data: Option<&[u8]>,
    ) -> Result<()> {
        // Validate dimensions
        if width == 0 || height == 0 {
            warn!(
                "CreateTexture2D: invalid dimensions {}x{} for id={}",
                width, height, id
            );
            return Err(anyhow!("Invalid texture dimensions"));
        }

        // D3D11 max texture size is 16384x16384
        if width > 16384 || height > 16384 {
            warn!(
                "CreateTexture2D: dimensions {}x{} exceed max (16384) for id={}",
                width, height, id
            );
            return Err(anyhow!("Texture dimensions exceed maximum"));
        }

        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: format,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: bind_flags,
            CPUAccessFlags: Default::default(),
            MiscFlags: Default::default(),
        };

        let init_data = initial_data.map(|data| D3D11_SUBRESOURCE_DATA {
            pSysMem: data.as_ptr() as *const _,
            SysMemPitch: width * 4, // Assuming 4 bytes per pixel
            SysMemSlicePitch: 0,
        });

        let mut texture: Option<ID3D11Texture2D> = None;
        let result = unsafe {
            self.device.CreateTexture2D(
                &desc,
                init_data.as_ref().map(|d| d as *const _),
                Some(&mut texture),
            )
        };

        match result {
            Ok(()) => {}
            Err(e) => {
                // Check for out-of-memory errors (E_OUTOFMEMORY = 0x8007000E)
                let hr = e.code().0 as u32;
                if hr == 0x8007000E {
                    warn!(
                        "CreateTexture2D OUT OF MEMORY: id={}, {}x{}, format={:?}",
                        id, width, height, format
                    );
                    return Err(anyhow!("OutOfMemory: texture creation failed"));
                }
                warn!(
                    "CreateTexture2D FAILED: id={}, {}x{}, format={:?}, error={:?}",
                    id, width, height, format, e
                );
                return Err(anyhow!("Texture creation failed: {:?}", e));
            }
        }

        let texture = texture.ok_or_else(|| anyhow!("Failed to create texture"))?;

        // Create SRV if shader resource bind flag is set
        let srv = if (bind_flags & D3D11_BIND_SHADER_RESOURCE.0 as u32) != 0 {
            let mut srv: Option<ID3D11ShaderResourceView> = None;
            unsafe {
                self.device
                    .CreateShaderResourceView(&texture, None, Some(&mut srv))?;
            }
            srv
        } else {
            None
        };

        // Create RTV if render target bind flag is set
        let rtv = if (bind_flags & D3D11_BIND_RENDER_TARGET.0 as u32) != 0 {
            let mut rtv: Option<ID3D11RenderTargetView> = None;
            unsafe {
                self.device
                    .CreateRenderTargetView(&texture, None, Some(&mut rtv))?;
            }
            rtv
        } else {
            None
        };

        debug!(
            "Created Texture2D: id={}, {}x{}, format={:?}",
            id, width, height, format
        );

        self.slab_insert(
            id,
            D3D11Resource::Texture2D {
                texture,
                width,
                height,
                format,
                srv,
                rtv,
            },
        );

        Ok(())
    }

    /// Create a buffer (vertex, index, or constant buffer)
    pub fn create_buffer(
        &mut self,
        id: ResourceId,
        size: u32,
        bind_flags: u32,
        initial_data: Option<&[u8]>,
    ) -> Result<()> {
        // Validate size
        if size == 0 {
            warn!("CreateBuffer: invalid size 0 for id={}", id);
            return Err(anyhow!("Invalid buffer size"));
        }

        // D3D11 max buffer size is limited by available GPU memory
        // A reasonable sanity check is 1GB
        if size > 1024 * 1024 * 1024 {
            warn!(
                "CreateBuffer: size {} exceeds max (1GB) for id={}",
                size, id
            );
            return Err(anyhow!("Buffer size exceeds maximum"));
        }

        let desc = D3D11_BUFFER_DESC {
            ByteWidth: size,
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: bind_flags,
            CPUAccessFlags: Default::default(),
            MiscFlags: Default::default(),
            StructureByteStride: 0,
        };

        let init_data = initial_data.map(|data| D3D11_SUBRESOURCE_DATA {
            pSysMem: data.as_ptr() as *const _,
            SysMemPitch: 0,
            SysMemSlicePitch: 0,
        });

        let mut buffer: Option<ID3D11Buffer> = None;
        let result = unsafe {
            self.device.CreateBuffer(
                &desc,
                init_data.as_ref().map(|d| d as *const _),
                Some(&mut buffer),
            )
        };

        match result {
            Ok(()) => {}
            Err(e) => {
                // Check for out-of-memory errors (E_OUTOFMEMORY = 0x8007000E)
                let hr = e.code().0 as u32;
                if hr == 0x8007000E {
                    warn!(
                        "CreateBuffer OUT OF MEMORY: id={}, size={}, bind_flags={}",
                        id, size, bind_flags
                    );
                    return Err(anyhow!("OutOfMemory: buffer creation failed"));
                }
                warn!(
                    "CreateBuffer FAILED: id={}, size={}, bind_flags={}, error={:?}",
                    id, size, bind_flags, e
                );
                return Err(anyhow!("Buffer creation failed: {:?}", e));
            }
        }

        let buffer = buffer.ok_or_else(|| anyhow!("Failed to create buffer"))?;

        debug!(
            "Created Buffer: id={}, size={}, bind_flags={}",
            id, size, bind_flags
        );

        self.slab_insert(
            id,
            D3D11Resource::Buffer {
                buffer,
                size,
                bind_flags,
            },
        );

        Ok(())
    }

    /// Create a vertex shader from DXBC bytecode
    pub fn create_vertex_shader(&mut self, id: ResourceId, bytecode: &[u8]) -> Result<()> {
        if bytecode.is_empty() {
            warn!("CreateVertexShader: empty bytecode for id={}", id);
            return Err(anyhow!("Shader bytecode is empty"));
        }

        let mut shader: Option<ID3D11VertexShader> = None;
        let result = unsafe {
            self.device
                .CreateVertexShader(bytecode, None, Some(&mut shader))
        };

        match result {
            Ok(()) => {
                let shader = shader.ok_or_else(|| anyhow!("Failed to create vertex shader"))?;

                debug!(
                    "Created VertexShader: id={}, bytecode_size={}",
                    id,
                    bytecode.len()
                );

                self.slab_insert(
                    id,
                    D3D11Resource::VertexShader {
                        shader,
                        bytecode: bytecode.to_vec(),
                    },
                );

                Ok(())
            }
            Err(e) => {
                warn!(
                    "CreateVertexShader FAILED: id={}, bytecode_size={}, error={:?}",
                    id,
                    bytecode.len(),
                    e
                );
                Err(anyhow!("Vertex shader compilation failed: {:?}", e))
            }
        }
    }

    /// Create a pixel shader from DXBC bytecode
    pub fn create_pixel_shader(&mut self, id: ResourceId, bytecode: &[u8]) -> Result<()> {
        if bytecode.is_empty() {
            warn!("CreatePixelShader: empty bytecode for id={}", id);
            return Err(anyhow!("Shader bytecode is empty"));
        }

        let mut shader: Option<ID3D11PixelShader> = None;
        let result = unsafe {
            self.device
                .CreatePixelShader(bytecode, None, Some(&mut shader))
        };

        match result {
            Ok(()) => {
                let shader = shader.ok_or_else(|| anyhow!("Failed to create pixel shader"))?;

                debug!(
                    "Created PixelShader: id={}, bytecode_size={}",
                    id,
                    bytecode.len()
                );

                self.slab_insert(id, D3D11Resource::PixelShader { shader });

                Ok(())
            }
            Err(e) => {
                warn!(
                    "CreatePixelShader FAILED: id={}, bytecode_size={}, error={:?}",
                    id,
                    bytecode.len(),
                    e
                );
                Err(anyhow!("Pixel shader compilation failed: {:?}", e))
            }
        }
    }

    /// Create a geometry shader from DXBC bytecode
    pub fn create_geometry_shader(&mut self, id: ResourceId, bytecode: &[u8]) -> Result<()> {
        if bytecode.is_empty() {
            warn!("CreateGeometryShader: empty bytecode for id={}", id);
            return Err(anyhow!("Shader bytecode is empty"));
        }

        let mut shader: Option<ID3D11GeometryShader> = None;
        let result = unsafe {
            self.device
                .CreateGeometryShader(bytecode, None, Some(&mut shader))
        };

        match result {
            Ok(()) => {
                let shader = shader.ok_or_else(|| anyhow!("Failed to create geometry shader"))?;

                debug!(
                    "Created GeometryShader: id={}, bytecode_size={}",
                    id,
                    bytecode.len()
                );

                self.slab_insert(id, D3D11Resource::GeometryShader { shader });

                Ok(())
            }
            Err(e) => {
                warn!(
                    "CreateGeometryShader FAILED: id={}, bytecode_size={}, error={:?}",
                    id,
                    bytecode.len(),
                    e
                );
                Err(anyhow!("Geometry shader compilation failed: {:?}", e))
            }
        }
    }

    /// Create a hull shader from DXBC bytecode
    pub fn create_hull_shader(&mut self, id: ResourceId, bytecode: &[u8]) -> Result<()> {
        if bytecode.is_empty() {
            warn!("CreateHullShader: empty bytecode for id={}", id);
            return Err(anyhow!("Shader bytecode is empty"));
        }

        let mut shader: Option<ID3D11HullShader> = None;
        let result = unsafe {
            self.device
                .CreateHullShader(bytecode, None, Some(&mut shader))
        };

        match result {
            Ok(()) => {
                let shader = shader.ok_or_else(|| anyhow!("Failed to create hull shader"))?;

                debug!(
                    "Created HullShader: id={}, bytecode_size={}",
                    id,
                    bytecode.len()
                );

                self.slab_insert(id, D3D11Resource::HullShader { shader });

                Ok(())
            }
            Err(e) => {
                warn!(
                    "CreateHullShader FAILED: id={}, bytecode_size={}, error={:?}",
                    id,
                    bytecode.len(),
                    e
                );
                Err(anyhow!("Hull shader compilation failed: {:?}", e))
            }
        }
    }

    /// Create a domain shader from DXBC bytecode
    pub fn create_domain_shader(&mut self, id: ResourceId, bytecode: &[u8]) -> Result<()> {
        if bytecode.is_empty() {
            warn!("CreateDomainShader: empty bytecode for id={}", id);
            return Err(anyhow!("Shader bytecode is empty"));
        }

        let mut shader: Option<ID3D11DomainShader> = None;
        let result = unsafe {
            self.device
                .CreateDomainShader(bytecode, None, Some(&mut shader))
        };

        match result {
            Ok(()) => {
                let shader = shader.ok_or_else(|| anyhow!("Failed to create domain shader"))?;

                debug!(
                    "Created DomainShader: id={}, bytecode_size={}",
                    id,
                    bytecode.len()
                );

                self.slab_insert(id, D3D11Resource::DomainShader { shader });

                Ok(())
            }
            Err(e) => {
                warn!(
                    "CreateDomainShader FAILED: id={}, bytecode_size={}, error={:?}",
                    id,
                    bytecode.len(),
                    e
                );
                Err(anyhow!("Domain shader compilation failed: {:?}", e))
            }
        }
    }

    /// Create a compute shader from DXBC bytecode
    pub fn create_compute_shader(&mut self, id: ResourceId, bytecode: &[u8]) -> Result<()> {
        if bytecode.is_empty() {
            warn!("CreateComputeShader: empty bytecode for id={}", id);
            return Err(anyhow!("Shader bytecode is empty"));
        }

        let mut shader: Option<ID3D11ComputeShader> = None;
        let result = unsafe {
            self.device
                .CreateComputeShader(bytecode, None, Some(&mut shader))
        };

        match result {
            Ok(()) => {
                let shader = shader.ok_or_else(|| anyhow!("Failed to create compute shader"))?;

                debug!(
                    "Created ComputeShader: id={}, bytecode_size={}",
                    id,
                    bytecode.len()
                );

                self.slab_insert(id, D3D11Resource::ComputeShader { shader });

                Ok(())
            }
            Err(e) => {
                warn!(
                    "CreateComputeShader FAILED: id={}, bytecode_size={}, error={:?}",
                    id,
                    bytecode.len(),
                    e
                );
                Err(anyhow!("Compute shader compilation failed: {:?}", e))
            }
        }
    }

    /// Destroy a resource by ID
    pub fn destroy_resource(&mut self, id: ResourceId) -> bool {
        if self.slab_remove(id).is_some() {
            debug!("Destroyed resource {}", id);
            true
        } else {
            warn!("Attempted to destroy non-existent resource {}", id);
            false
        }
    }

    /// Get a resource by ID
    pub fn get_resource(&self, id: ResourceId) -> Option<&D3D11Resource> {
        self.slab_get(id)
    }

    /// Get a texture by ID (convenience method for presentation)
    pub fn get_texture(&self, id: ResourceId) -> Option<&ID3D11Texture2D> {
        match self.slab_get(id) {
            Some(D3D11Resource::Texture2D { texture, .. }) => Some(texture),
            _ => None,
        }
    }

    /// Get a buffer by ID
    pub fn get_buffer(&self, id: ResourceId) -> Option<&ID3D11Buffer> {
        match self.slab_get(id) {
            Some(D3D11Resource::Buffer { buffer, .. }) => Some(buffer),
            _ => None,
        }
    }

    /// Register an externally-created texture with a given resource ID
    pub fn register_texture(&mut self, id: ResourceId, texture: ID3D11Texture2D) {
        let mut desc = D3D11_TEXTURE2D_DESC::default();
        unsafe {
            texture.GetDesc(&mut desc);
        }
        self.slab_insert(
            id,
            D3D11Resource::Texture2D {
                texture,
                width: desc.Width,
                height: desc.Height,
                format: desc.Format,
                rtv: None,
                srv: None,
            },
        );
    }

    /// Register an externally-created buffer with a given resource ID
    pub fn register_buffer(&mut self, id: ResourceId, buffer: ID3D11Buffer) {
        // Query the buffer description to get size and bind flags
        let mut desc = D3D11_BUFFER_DESC::default();
        unsafe {
            buffer.GetDesc(&mut desc);
        }
        self.slab_insert(
            id,
            D3D11Resource::Buffer {
                buffer,
                size: desc.ByteWidth,
                bind_flags: desc.BindFlags,
            },
        );
    }

    /// Open a shared resource by DXGI shared handle and register it
    pub fn open_shared_texture(&mut self, id: ResourceId, shared_handle: u64) -> Result<()> {
        unsafe {
            let handle = windows::Win32::Foundation::HANDLE(shared_handle as *mut std::ffi::c_void);
            let mut texture: Option<ID3D11Texture2D> = None;
            self.device.OpenSharedResource(handle, &mut texture)?;
            let texture = texture.ok_or_else(|| anyhow!("OpenSharedResource returned null"))?;
            self.register_texture(id, texture);
            Ok(())
        }
    }

    /// Get the DXGI factory
    pub fn factory(&self) -> &IDXGIFactory1 {
        &self.factory
    }

    /// Set render targets
    pub fn set_render_targets(
        &mut self,
        rtv_ids: &[ResourceId],
        dsv_id: Option<ResourceId>,
    ) -> Result<()> {
        // Collect RTVs
        let mut rtvs: Vec<Option<ID3D11RenderTargetView>> = Vec::new();
        for &id in rtv_ids {
            if id == 0 {
                rtvs.push(None);
            } else if let Some(D3D11Resource::Texture2D { rtv, .. }) = self.slab_get(id) {
                rtvs.push(rtv.clone());
            } else if let Some(D3D11Resource::RenderTargetView { rtv }) = self.slab_get(id) {
                rtvs.push(Some(rtv.clone()));
            } else {
                return Err(anyhow!("Invalid RTV resource ID: {}", id));
            }
        }

        // Get DSV
        let dsv = if let Some(id) = dsv_id {
            if id == 0 {
                None
            } else if let Some(D3D11Resource::DepthStencilView { dsv }) = self.slab_get(id) {
                Some(dsv.clone())
            } else {
                return Err(anyhow!("Invalid DSV resource ID: {}", id));
            }
        } else {
            None
        };

        // Set on context
        unsafe {
            self.context.OMSetRenderTargets(Some(&rtvs), dsv.as_ref());
        }

        self.current_rtvs = rtvs;
        self.current_dsv = dsv;

        Ok(())
    }

    /// Set viewports
    pub fn set_viewports(&mut self, viewports: &[D3D11_VIEWPORT]) {
        unsafe {
            self.context.RSSetViewports(Some(viewports));
        }
    }

    /// Execute a draw call
    pub fn draw(&mut self, vertex_count: u32, start_vertex: u32) {
        debug!("Draw: {} vertices from {}", vertex_count, start_vertex);
        unsafe {
            self.context.Draw(vertex_count, start_vertex);
        }
    }

    /// Execute an indexed draw call
    pub fn draw_indexed(&mut self, index_count: u32, start_index: u32, base_vertex: i32) {
        debug!(
            "DrawIndexed: {} indices from {}, base {}",
            index_count, start_index, base_vertex
        );
        unsafe {
            self.context
                .DrawIndexed(index_count, start_index, base_vertex);
        }
    }

    /// Clear a render target view
    pub fn clear_render_target(&mut self, rtv_id: ResourceId, color: &[f32; 4]) {
        if let Some(D3D11Resource::Texture2D { rtv: Some(rtv), .. }) = self.slab_get(rtv_id) {
            unsafe {
                self.context.ClearRenderTargetView(rtv, color);
            }
        } else if let Some(D3D11Resource::RenderTargetView { rtv }) = self.slab_get(rtv_id) {
            unsafe {
                self.context.ClearRenderTargetView(rtv, color);
            }
        } else {
            warn!("ClearRenderTarget: Invalid RTV ID {}", rtv_id);
        }
    }

    /// Flush pending commands
    pub fn flush(&mut self) {
        unsafe {
            self.context.Flush();
        }
    }

    /// Flush and signal that a frame is ready for presentation.
    ///
    /// The actual presentation is handled by the PresentationPipeline in the
    /// main loop via the `pending_present` mechanism. This method just ensures
    /// the GPU command queue is flushed before the presentation pipeline copies
    /// the backbuffer to the swapchain.
    pub fn present(&mut self, backbuffer_id: ResourceId, sync_interval: u32) {
        debug!(
            "Present: backbuffer {}, sync {}",
            backbuffer_id, sync_interval
        );
        self.flush();
    }

    // =========================================================================
    // State Commands
    // =========================================================================

    /// Set a vertex buffer to an input slot
    pub fn set_vertex_buffer(
        &mut self,
        slot: u32,
        buffer_id: ResourceId,
        stride: u32,
        offset: u32,
    ) {
        if buffer_id == 0 {
            // Unbind
            let buffers: [Option<ID3D11Buffer>; 1] = [None];
            let strides: [u32; 1] = [stride];
            let offsets: [u32; 1] = [offset];
            unsafe {
                self.context.IASetVertexBuffers(
                    slot,
                    1,
                    Some(buffers.as_ptr()),
                    Some(strides.as_ptr()),
                    Some(offsets.as_ptr()),
                );
            }
            return;
        }

        if let Some(D3D11Resource::Buffer { buffer, .. }) = self.slab_get(buffer_id) {
            debug!(
                "SetVertexBuffer: slot={}, buffer={}, stride={}, offset={}",
                slot, buffer_id, stride, offset
            );
            let buffers: [Option<ID3D11Buffer>; 1] = [Some(buffer.clone())];
            let strides: [u32; 1] = [stride];
            let offsets: [u32; 1] = [offset];
            unsafe {
                self.context.IASetVertexBuffers(
                    slot,
                    1,
                    Some(buffers.as_ptr()),
                    Some(strides.as_ptr()),
                    Some(offsets.as_ptr()),
                );
            }
        } else {
            warn!("SetVertexBuffer: Invalid buffer ID {}", buffer_id);
        }
    }

    /// Set the index buffer
    pub fn set_index_buffer(&mut self, buffer_id: ResourceId, format: DXGI_FORMAT, offset: u32) {
        if buffer_id == 0 {
            // Unbind
            unsafe {
                self.context.IASetIndexBuffer(None, format, offset);
            }
            return;
        }

        if let Some(D3D11Resource::Buffer { buffer, .. }) = self.slab_get(buffer_id) {
            debug!(
                "SetIndexBuffer: buffer={}, format={:?}, offset={}",
                buffer_id, format, offset
            );
            unsafe {
                self.context.IASetIndexBuffer(buffer, format, offset);
            }
        } else {
            warn!("SetIndexBuffer: Invalid buffer ID {}", buffer_id);
        }
    }

    /// Set a constant buffer for a shader stage
    pub fn set_constant_buffer(&mut self, stage: u32, slot: u32, buffer_id: ResourceId) {
        let buffer = if buffer_id == 0 {
            None
        } else if let Some(D3D11Resource::Buffer { buffer, .. }) = self.slab_get(buffer_id) {
            Some(buffer.clone())
        } else {
            warn!("SetConstantBuffer: Invalid buffer ID {}", buffer_id);
            return;
        };

        debug!(
            "SetConstantBuffer: stage={}, slot={}, buffer={}",
            stage, slot, buffer_id
        );

        let buffers = [buffer];
        unsafe {
            match stage {
                0 => self.context.VSSetConstantBuffers(slot, Some(&buffers)),
                1 => self.context.PSSetConstantBuffers(slot, Some(&buffers)),
                2 => self.context.GSSetConstantBuffers(slot, Some(&buffers)),
                3 => self.context.HSSetConstantBuffers(slot, Some(&buffers)),
                4 => self.context.DSSetConstantBuffers(slot, Some(&buffers)),
                5 => self.context.CSSetConstantBuffers(slot, Some(&buffers)),
                _ => warn!("SetConstantBuffer: Unknown stage {}", stage),
            }
        }
    }

    /// Set the input layout
    pub fn set_input_layout(&mut self, layout_id: ResourceId) {
        if layout_id == 0 {
            unsafe {
                self.context.IASetInputLayout(None);
            }
            return;
        }

        if let Some(D3D11Resource::InputLayout { layout }) = self.slab_get(layout_id) {
            debug!("SetInputLayout: layout={}", layout_id);
            unsafe {
                self.context.IASetInputLayout(layout);
            }
        } else {
            warn!("SetInputLayout: Invalid layout ID {}", layout_id);
        }
    }

    /// Set the primitive topology
    pub fn set_primitive_topology(&mut self, topology: u32) {
        debug!("SetPrimitiveTopology: topology={}", topology);
        unsafe {
            self.context
                .IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY(topology as i32));
        }
    }

    /// Set a sampler for a shader stage
    pub fn set_sampler(&mut self, stage: u32, slot: u32, sampler_id: ResourceId) {
        let sampler = if sampler_id == 0 {
            None
        } else if let Some(D3D11Resource::SamplerState { state }) = self.slab_get(sampler_id) {
            Some(state.clone())
        } else {
            warn!("SetSampler: Invalid sampler ID {}", sampler_id);
            return;
        };

        debug!(
            "SetSampler: stage={}, slot={}, sampler={}",
            stage, slot, sampler_id
        );

        let samplers = [sampler];
        unsafe {
            match stage {
                0 => self.context.VSSetSamplers(slot, Some(&samplers)),
                1 => self.context.PSSetSamplers(slot, Some(&samplers)),
                2 => self.context.GSSetSamplers(slot, Some(&samplers)),
                3 => self.context.HSSetSamplers(slot, Some(&samplers)),
                4 => self.context.DSSetSamplers(slot, Some(&samplers)),
                5 => self.context.CSSetSamplers(slot, Some(&samplers)),
                _ => warn!("SetSampler: Unknown stage {}", stage),
            }
        }
    }

    /// Set a shader resource view for a shader stage
    pub fn set_shader_resource(&mut self, stage: u32, slot: u32, srv_id: ResourceId) {
        let srv = if srv_id == 0 {
            None
        } else if let Some(D3D11Resource::Texture2D { srv: Some(srv), .. }) = self.slab_get(srv_id)
        {
            Some(srv.clone())
        } else if let Some(D3D11Resource::ShaderResourceView { srv }) = self.slab_get(srv_id) {
            Some(srv.clone())
        } else {
            warn!("SetShaderResource: Invalid SRV ID {}", srv_id);
            return;
        };

        debug!(
            "SetShaderResource: stage={}, slot={}, srv={}",
            stage, slot, srv_id
        );

        let srvs = [srv];
        unsafe {
            match stage {
                0 => self.context.VSSetShaderResources(slot, Some(&srvs)),
                1 => self.context.PSSetShaderResources(slot, Some(&srvs)),
                2 => self.context.GSSetShaderResources(slot, Some(&srvs)),
                3 => self.context.HSSetShaderResources(slot, Some(&srvs)),
                4 => self.context.DSSetShaderResources(slot, Some(&srvs)),
                5 => self.context.CSSetShaderResources(slot, Some(&srvs)),
                _ => warn!("SetShaderResource: Unknown stage {}", stage),
            }
        }
    }

    /// Set the blend state
    pub fn set_blend_state(
        &mut self,
        state_id: ResourceId,
        blend_factor: &[f32; 4],
        sample_mask: u32,
    ) {
        if state_id == 0 {
            unsafe {
                self.context
                    .OMSetBlendState(None, Some(blend_factor), sample_mask);
            }
            return;
        }

        if let Some(D3D11Resource::BlendState { state }) = self.slab_get(state_id) {
            debug!("SetBlendState: state={}", state_id);
            unsafe {
                self.context
                    .OMSetBlendState(state, Some(blend_factor), sample_mask);
            }
        } else {
            warn!("SetBlendState: Invalid state ID {}", state_id);
        }
    }

    /// Set the rasterizer state
    pub fn set_rasterizer_state(&mut self, state_id: ResourceId) {
        if state_id == 0 {
            unsafe {
                self.context.RSSetState(None);
            }
            return;
        }

        if let Some(D3D11Resource::RasterizerState { state }) = self.slab_get(state_id) {
            debug!("SetRasterizerState: state={}", state_id);
            unsafe {
                self.context.RSSetState(state);
            }
        } else {
            warn!("SetRasterizerState: Invalid state ID {}", state_id);
        }
    }

    /// Set the depth-stencil state
    pub fn set_depth_stencil_state(&mut self, state_id: ResourceId, stencil_ref: u32) {
        if state_id == 0 {
            unsafe {
                self.context.OMSetDepthStencilState(None, stencil_ref);
            }
            return;
        }

        if let Some(D3D11Resource::DepthStencilState { state }) = self.slab_get(state_id) {
            debug!(
                "SetDepthStencilState: state={}, ref={}",
                state_id, stencil_ref
            );
            unsafe {
                self.context.OMSetDepthStencilState(state, stencil_ref);
            }
        } else {
            warn!("SetDepthStencilState: Invalid state ID {}", state_id);
        }
    }

    /// Set scissor rectangles
    pub fn set_scissor_rects(&mut self, rects: &[windows::Win32::Foundation::RECT]) {
        debug!("SetScissorRects: {} rects", rects.len());
        unsafe {
            self.context.RSSetScissorRects(Some(rects));
        }
    }

    /// Set a shader
    pub fn set_shader(&mut self, stage: u32, shader_id: ResourceId) {
        if shader_id == 0 {
            // Unbind shader
            debug!("SetShader: stage={}, unbinding", stage);
            unsafe {
                match stage {
                    0 => self.context.VSSetShader(None, None),
                    1 => self.context.PSSetShader(None, None),
                    2 => self.context.GSSetShader(None, None),
                    3 => self.context.HSSetShader(None, None),
                    4 => self.context.DSSetShader(None, None),
                    5 => self.context.CSSetShader(None, None),
                    _ => warn!("SetShader: Unknown stage {}", stage),
                }
            }
            return;
        }

        debug!("SetShader: stage={}, shader={}", stage, shader_id);

        match stage {
            0 => {
                if let Some(D3D11Resource::VertexShader { shader, .. }) = self.slab_get(shader_id) {
                    unsafe {
                        self.context.VSSetShader(shader, None);
                    }
                } else {
                    warn!("SetShader: Invalid vertex shader ID {}", shader_id);
                }
            }
            1 => {
                if let Some(D3D11Resource::PixelShader { shader }) = self.slab_get(shader_id) {
                    unsafe {
                        self.context.PSSetShader(shader, None);
                    }
                } else {
                    warn!("SetShader: Invalid pixel shader ID {}", shader_id);
                }
            }
            2 => {
                if let Some(D3D11Resource::GeometryShader { shader }) = self.slab_get(shader_id) {
                    unsafe {
                        self.context.GSSetShader(shader, None);
                    }
                } else {
                    warn!("SetShader: Invalid geometry shader ID {}", shader_id);
                }
            }
            3 => {
                if let Some(D3D11Resource::HullShader { shader }) = self.slab_get(shader_id) {
                    unsafe {
                        self.context.HSSetShader(shader, None);
                    }
                } else {
                    warn!("SetShader: Invalid hull shader ID {}", shader_id);
                }
            }
            4 => {
                if let Some(D3D11Resource::DomainShader { shader }) = self.slab_get(shader_id) {
                    unsafe {
                        self.context.DSSetShader(shader, None);
                    }
                } else {
                    warn!("SetShader: Invalid domain shader ID {}", shader_id);
                }
            }
            5 => {
                if let Some(D3D11Resource::ComputeShader { shader }) = self.slab_get(shader_id) {
                    unsafe {
                        self.context.CSSetShader(shader, None);
                    }
                } else {
                    warn!("SetShader: Invalid compute shader ID {}", shader_id);
                }
            }
            _ => {
                warn!("SetShader: Unknown stage {}", stage);
            }
        }
    }

    // =========================================================================
    // Advanced Draw Commands
    // =========================================================================

    /// Draw instanced primitives
    pub fn draw_instanced(
        &mut self,
        vertex_count: u32,
        instance_count: u32,
        start_vertex: u32,
        start_instance: u32,
    ) {
        debug!(
            "DrawInstanced: {} vertices, {} instances",
            vertex_count, instance_count
        );
        unsafe {
            self.context
                .DrawInstanced(vertex_count, instance_count, start_vertex, start_instance);
        }
    }

    /// Draw indexed, instanced primitives
    pub fn draw_indexed_instanced(
        &mut self,
        index_count: u32,
        instance_count: u32,
        start_index: u32,
        base_vertex: i32,
        start_instance: u32,
    ) {
        debug!(
            "DrawIndexedInstanced: {} indices, {} instances",
            index_count, instance_count
        );
        unsafe {
            self.context.DrawIndexedInstanced(
                index_count,
                instance_count,
                start_index,
                base_vertex,
                start_instance,
            );
        }
    }

    /// Dispatch a compute shader
    pub fn dispatch(&mut self, x: u32, y: u32, z: u32) {
        debug!("Dispatch: {}x{}x{}", x, y, z);
        unsafe {
            self.context.Dispatch(x, y, z);
        }
    }

    /// Clear a depth-stencil view
    pub fn clear_depth_stencil(
        &mut self,
        dsv_id: ResourceId,
        clear_flags: u32,
        depth: f32,
        stencil: u8,
    ) {
        if let Some(D3D11Resource::DepthStencilView { dsv }) = self.slab_get(dsv_id) {
            debug!(
                "ClearDepthStencil: dsv={}, flags={}, depth={}, stencil={}",
                dsv_id, clear_flags, depth, stencil
            );
            unsafe {
                // clear_flags: 1 = D3D11_CLEAR_DEPTH, 2 = D3D11_CLEAR_STENCIL
                self.context
                    .ClearDepthStencilView(dsv, clear_flags, depth, stencil);
            }
        } else {
            warn!("ClearDepthStencil: Invalid DSV ID {}", dsv_id);
        }
    }

    /// Copy entire resource
    pub fn copy_resource(&mut self, dst_id: ResourceId, src_id: ResourceId) {
        let src_resource: Option<ID3D11Resource> = match self.slab_get(src_id) {
            Some(D3D11Resource::Texture2D { texture, .. }) => texture.cast().ok(),
            Some(D3D11Resource::Buffer { buffer, .. }) => buffer.cast().ok(),
            _ => None,
        };

        let dst_resource: Option<ID3D11Resource> = match self.slab_get(dst_id) {
            Some(D3D11Resource::Texture2D { texture, .. }) => texture.cast().ok(),
            Some(D3D11Resource::Buffer { buffer, .. }) => buffer.cast().ok(),
            _ => None,
        };

        if let (Some(dst), Some(src)) = (dst_resource, src_resource) {
            debug!("CopyResource: dst={}, src={}", dst_id, src_id);
            unsafe {
                self.context.CopyResource(&dst, &src);
            }
        } else {
            warn!(
                "CopyResource: Invalid resource IDs dst={} src={}",
                dst_id, src_id
            );
        }
    }

    // =========================================================================
    // Resource Data Transfer
    // =========================================================================

    /// Map a resource for CPU access.
    /// Returns the mapped data pointer and row pitch for textures.
    /// For D3D11_USAGE_DEFAULT resources (most common), this uses staging buffers.
    pub fn map_resource(
        &mut self,
        id: ResourceId,
        subresource: u32,
        map_type: u32,
    ) -> Result<MapResult> {
        use windows::Win32::Graphics::Direct3D11::{
            D3D11_CPU_ACCESS_READ, D3D11_CPU_ACCESS_WRITE, D3D11_MAP, D3D11_MAPPED_SUBRESOURCE,
            D3D11_TEXTURE2D_DESC, D3D11_USAGE_STAGING,
        };

        let resource = self.slab_get(id);

        match resource {
            Some(D3D11Resource::Buffer { buffer, size, .. }) => {
                // For DEFAULT usage buffers, create a staging buffer
                let staging_desc = D3D11_BUFFER_DESC {
                    ByteWidth: *size,
                    Usage: D3D11_USAGE_STAGING,
                    BindFlags: Default::default(),
                    CPUAccessFlags: (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE).0 as u32,
                    MiscFlags: Default::default(),
                    StructureByteStride: 0,
                };

                let mut staging_buffer: Option<ID3D11Buffer> = None;
                unsafe {
                    self.device
                        .CreateBuffer(&staging_desc, None, Some(&mut staging_buffer))?;
                }
                let staging =
                    staging_buffer.ok_or_else(|| anyhow!("Failed to create staging buffer"))?;

                // Copy from source if reading
                let d3d_map_type = D3D11_MAP(map_type as i32);
                if map_type == 1 || map_type == 3 {
                    // Read or ReadWrite
                    unsafe {
                        self.context.CopyResource(&staging, buffer);
                    }
                }

                // Map the staging buffer
                let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
                unsafe {
                    self.context
                        .Map(&staging, 0, d3d_map_type, 0, Some(&mut mapped))?;
                }

                debug!(
                    "MapResource: id={}, subresource={}, type={}, size={}",
                    id, subresource, map_type, *size
                );

                Ok(MapResult {
                    data_ptr: mapped.pData as *mut u8,
                    row_pitch: mapped.RowPitch,
                    depth_pitch: mapped.DepthPitch,
                    size: *size as usize,
                    staging_resource: Some(StagingResource::Buffer(staging)),
                    original_buffer: Some(buffer.clone()),
                    original_texture: None,
                })
            }
            Some(D3D11Resource::Texture2D {
                texture,
                width,
                height,
                format,
                ..
            }) => {
                // Get the texture description
                let mut desc = D3D11_TEXTURE2D_DESC::default();
                unsafe {
                    texture.GetDesc(&mut desc);
                }

                // Create staging texture
                let staging_desc = D3D11_TEXTURE2D_DESC {
                    Width: *width,
                    Height: *height,
                    MipLevels: desc.MipLevels,
                    ArraySize: desc.ArraySize,
                    Format: *format,
                    SampleDesc: DXGI_SAMPLE_DESC {
                        Count: 1,
                        Quality: 0,
                    },
                    Usage: D3D11_USAGE_STAGING,
                    BindFlags: Default::default(),
                    CPUAccessFlags: (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE).0 as u32,
                    MiscFlags: Default::default(),
                };

                let mut staging_texture: Option<ID3D11Texture2D> = None;
                unsafe {
                    self.device
                        .CreateTexture2D(&staging_desc, None, Some(&mut staging_texture))?;
                }
                let staging =
                    staging_texture.ok_or_else(|| anyhow!("Failed to create staging texture"))?;

                // Copy from source if reading
                let d3d_map_type = D3D11_MAP(map_type as i32);
                if map_type == 1 || map_type == 3 {
                    // Read or ReadWrite
                    unsafe {
                        self.context.CopyResource(&staging, texture);
                    }
                }

                // Map the staging texture
                let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
                unsafe {
                    self.context
                        .Map(&staging, subresource, d3d_map_type, 0, Some(&mut mapped))?;
                }

                // Calculate approximate size (row pitch * height for 2D textures)
                let size = (mapped.RowPitch * *height) as usize;

                debug!(
                    "MapResource: id={}, subresource={}, type={}, {}x{}, pitch={}",
                    id, subresource, map_type, width, height, mapped.RowPitch
                );

                Ok(MapResult {
                    data_ptr: mapped.pData as *mut u8,
                    row_pitch: mapped.RowPitch,
                    depth_pitch: mapped.DepthPitch,
                    size,
                    staging_resource: Some(StagingResource::Texture2D(staging)),
                    original_buffer: None,
                    original_texture: Some(texture.clone()),
                })
            }
            _ => Err(anyhow!(
                "MapResource: Invalid or unsupported resource ID {}",
                id
            )),
        }
    }

    /// Unmap a previously mapped resource.
    /// If the resource was mapped for writing, copies data back to the GPU resource.
    pub fn unmap_resource(&mut self, map_result: &MapResult, subresource: u32, was_write: bool) {
        // Unmap the staging resource
        if let Some(ref staging) = map_result.staging_resource {
            match staging {
                StagingResource::Buffer(staging_buffer) => {
                    unsafe {
                        self.context.Unmap(staging_buffer, 0);
                    }
                    // Copy back if it was a write operation
                    if was_write {
                        if let Some(ref original) = map_result.original_buffer {
                            unsafe {
                                self.context.CopyResource(original, staging_buffer);
                            }
                            debug!("UnmapResource: copied buffer data back to GPU");
                        }
                    }
                }
                StagingResource::Texture2D(staging_texture) => {
                    unsafe {
                        self.context.Unmap(staging_texture, subresource);
                    }
                    // Copy back if it was a write operation
                    if was_write {
                        if let Some(ref original) = map_result.original_texture {
                            unsafe {
                                self.context.CopyResource(original, staging_texture);
                            }
                            debug!("UnmapResource: copied texture data back to GPU");
                        }
                    }
                }
            }
        }
    }

    /// Update a subresource with data from CPU memory.
    /// This is more efficient than Map/Unmap for write-only updates.
    pub fn update_subresource(
        &mut self,
        id: ResourceId,
        subresource: u32,
        data: &[u8],
        dst_box: Option<UpdateBox>,
        row_pitch: u32,
        depth_pitch: u32,
    ) -> Result<()> {
        use windows::Win32::Graphics::Direct3D11::D3D11_BOX;

        let resource = self.slab_get(id);

        let d3d_resource: Option<ID3D11Resource> = match resource {
            Some(D3D11Resource::Texture2D { texture, .. }) => texture.cast().ok(),
            Some(D3D11Resource::Buffer { buffer, .. }) => buffer.cast().ok(),
            _ => None,
        };

        let d3d_resource =
            d3d_resource.ok_or_else(|| anyhow!("UpdateSubresource: Invalid resource ID {}", id))?;

        let d3d_box = dst_box.map(|b| D3D11_BOX {
            left: b.left,
            top: b.top,
            front: b.front,
            right: b.right,
            bottom: b.bottom,
            back: b.back,
        });

        debug!(
            "UpdateSubresource: id={}, subresource={}, size={}, row_pitch={}, depth_pitch={}",
            id,
            subresource,
            data.len(),
            row_pitch,
            depth_pitch
        );

        unsafe {
            self.context.UpdateSubresource(
                &d3d_resource,
                subresource,
                d3d_box.as_ref().map(|b| b as *const _),
                data.as_ptr() as *const _,
                row_pitch,
                depth_pitch,
            );
        }

        Ok(())
    }
}

/// Result of mapping a resource
pub struct MapResult {
    pub data_ptr: *mut u8,
    pub row_pitch: u32,
    pub depth_pitch: u32,
    pub size: usize,
    pub staging_resource: Option<StagingResource>,
    pub original_buffer: Option<ID3D11Buffer>,
    pub original_texture: Option<ID3D11Texture2D>,
}

/// Staging resource used for Map/Unmap operations
pub enum StagingResource {
    Buffer(ID3D11Buffer),
    Texture2D(ID3D11Texture2D),
}

/// Box for partial updates
#[derive(Debug, Clone, Copy)]
pub struct UpdateBox {
    pub left: u32,
    pub top: u32,
    pub front: u32,
    pub right: u32,
    pub bottom: u32,
    pub back: u32,
}
