//! D3D11 Renderer Module
//!
//! Handles D3D11 device creation, resource management, and command execution.
//! This module wraps Direct3D 11 APIs to execute graphics commands received
//! from the guest via the command ring.

use std::collections::HashMap;

use anyhow::{anyhow, Result};
use tracing::{debug, info, warn};
use windows::Win32::Graphics::Direct3D::{
    D3D_DRIVER_TYPE_UNKNOWN, D3D_FEATURE_LEVEL, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
};
use windows::Win32::Graphics::Direct3D11::{
    D3D11CreateDevice, ID3D11BlendState, ID3D11Buffer, ID3D11DepthStencilState,
    ID3D11DepthStencilView, ID3D11Device, ID3D11DeviceContext, ID3D11InputLayout,
    ID3D11PixelShader, ID3D11RasterizerState, ID3D11RenderTargetView, ID3D11SamplerState,
    ID3D11ShaderResourceView, ID3D11Texture2D, ID3D11VertexShader, D3D11_BIND_RENDER_TARGET,
    D3D11_BIND_SHADER_RESOURCE, D3D11_BUFFER_DESC, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    D3D11_CREATE_DEVICE_DEBUG, D3D11_SDK_VERSION, D3D11_SUBRESOURCE_DATA, D3D11_TEXTURE2D_DESC,
    D3D11_USAGE_DEFAULT, D3D11_VIEWPORT,
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
    /// Resource map: guest resource ID → D3D11 resource
    resources: HashMap<ResourceId, D3D11Resource>,
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
        let mut flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        #[cfg(debug_assertions)]
        {
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        }

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
            resources: HashMap::new(),
            current_rtvs: vec![None; 8],
            current_dsv: None,
        })
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
        unsafe {
            self.device.CreateTexture2D(
                &desc,
                init_data.as_ref().map(|d| d as *const _),
                Some(&mut texture),
            )?;
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

        self.resources.insert(
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
        unsafe {
            self.device.CreateBuffer(
                &desc,
                init_data.as_ref().map(|d| d as *const _),
                Some(&mut buffer),
            )?;
        }

        let buffer = buffer.ok_or_else(|| anyhow!("Failed to create buffer"))?;

        debug!(
            "Created Buffer: id={}, size={}, bind_flags={}",
            id, size, bind_flags
        );

        self.resources.insert(
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
        let mut shader: Option<ID3D11VertexShader> = None;
        unsafe {
            self.device
                .CreateVertexShader(bytecode, None, Some(&mut shader))?;
        }

        let shader = shader.ok_or_else(|| anyhow!("Failed to create vertex shader"))?;

        debug!(
            "Created VertexShader: id={}, bytecode_size={}",
            id,
            bytecode.len()
        );

        self.resources.insert(
            id,
            D3D11Resource::VertexShader {
                shader,
                bytecode: bytecode.to_vec(),
            },
        );

        Ok(())
    }

    /// Create a pixel shader from DXBC bytecode
    pub fn create_pixel_shader(&mut self, id: ResourceId, bytecode: &[u8]) -> Result<()> {
        let mut shader: Option<ID3D11PixelShader> = None;
        unsafe {
            self.device
                .CreatePixelShader(bytecode, None, Some(&mut shader))?;
        }

        let shader = shader.ok_or_else(|| anyhow!("Failed to create pixel shader"))?;

        debug!(
            "Created PixelShader: id={}, bytecode_size={}",
            id,
            bytecode.len()
        );

        self.resources
            .insert(id, D3D11Resource::PixelShader { shader });

        Ok(())
    }

    /// Destroy a resource by ID
    pub fn destroy_resource(&mut self, id: ResourceId) -> bool {
        if self.resources.remove(&id).is_some() {
            debug!("Destroyed resource {}", id);
            true
        } else {
            warn!("Attempted to destroy non-existent resource {}", id);
            false
        }
    }

    /// Get a resource by ID
    pub fn get_resource(&self, id: ResourceId) -> Option<&D3D11Resource> {
        self.resources.get(&id)
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
            } else if let Some(D3D11Resource::Texture2D { rtv, .. }) = self.resources.get(&id) {
                rtvs.push(rtv.clone());
            } else if let Some(D3D11Resource::RenderTargetView { rtv }) = self.resources.get(&id) {
                rtvs.push(Some(rtv.clone()));
            } else {
                return Err(anyhow!("Invalid RTV resource ID: {}", id));
            }
        }

        // Get DSV
        let dsv = if let Some(id) = dsv_id {
            if id == 0 {
                None
            } else if let Some(D3D11Resource::DepthStencilView { dsv }) = self.resources.get(&id) {
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
        if let Some(D3D11Resource::Texture2D { rtv: Some(rtv), .. }) = self.resources.get(&rtv_id) {
            unsafe {
                self.context.ClearRenderTargetView(rtv, color);
            }
        } else if let Some(D3D11Resource::RenderTargetView { rtv }) = self.resources.get(&rtv_id) {
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

    /// Present the current frame (for now, just flush)
    pub fn present(&mut self, backbuffer_id: ResourceId, sync_interval: u32) {
        debug!(
            "Present: backbuffer {}, sync {}",
            backbuffer_id, sync_interval
        );
        // TODO: Copy to presentation pipeline swapchain
        self.flush();
    }
}
