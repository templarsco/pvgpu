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
    D3D11_BIND_SHADER_RESOURCE, D3D11_BUFFER_DESC, D3D11_CLEAR_DEPTH, D3D11_CLEAR_STENCIL,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_CREATE_DEVICE_DEBUG, D3D11_PRIMITIVE_TOPOLOGY,
    D3D11_SDK_VERSION, D3D11_SUBRESOURCE_DATA, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT,
    D3D11_VIEWPORT,
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

    /// Get a texture by ID (convenience method for presentation)
    pub fn get_texture(&self, id: ResourceId) -> Option<&ID3D11Texture2D> {
        match self.resources.get(&id) {
            Some(D3D11Resource::Texture2D { texture, .. }) => Some(texture),
            _ => None,
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

    // =========================================================================
    // State Commands
    // =========================================================================

    /// Set a vertex buffer to an input slot
    pub fn set_vertex_buffer(&mut self, slot: u32, buffer_id: ResourceId, stride: u32, offset: u32) {
        if buffer_id == 0 {
            // Unbind
            unsafe {
                self.context.IASetVertexBuffers(slot, Some(&[None]), Some(&[stride]), Some(&[offset]));
            }
            return;
        }

        if let Some(D3D11Resource::Buffer { buffer, .. }) = self.resources.get(&buffer_id) {
            debug!(
                "SetVertexBuffer: slot={}, buffer={}, stride={}, offset={}",
                slot, buffer_id, stride, offset
            );
            unsafe {
                self.context.IASetVertexBuffers(
                    slot,
                    Some(&[Some(buffer.clone())]),
                    Some(&[stride]),
                    Some(&[offset]),
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

        if let Some(D3D11Resource::Buffer { buffer, .. }) = self.resources.get(&buffer_id) {
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
        } else if let Some(D3D11Resource::Buffer { buffer, .. }) = self.resources.get(&buffer_id) {
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

        if let Some(D3D11Resource::InputLayout { layout }) = self.resources.get(&layout_id) {
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
                .IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(topology as i32));
        }
    }

    /// Set a sampler for a shader stage
    pub fn set_sampler(&mut self, stage: u32, slot: u32, sampler_id: ResourceId) {
        let sampler = if sampler_id == 0 {
            None
        } else if let Some(D3D11Resource::SamplerState { state }) = self.resources.get(&sampler_id)
        {
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
        } else if let Some(D3D11Resource::Texture2D { srv: Some(srv), .. }) =
            self.resources.get(&srv_id)
        {
            Some(srv.clone())
        } else if let Some(D3D11Resource::ShaderResourceView { srv }) = self.resources.get(&srv_id)
        {
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

        if let Some(D3D11Resource::BlendState { state }) = self.resources.get(&state_id) {
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

        if let Some(D3D11Resource::RasterizerState { state }) = self.resources.get(&state_id) {
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

        if let Some(D3D11Resource::DepthStencilState { state }) = self.resources.get(&state_id) {
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
                if let Some(D3D11Resource::VertexShader { shader, .. }) =
                    self.resources.get(&shader_id)
                {
                    unsafe {
                        self.context.VSSetShader(shader, None);
                    }
                } else {
                    warn!("SetShader: Invalid vertex shader ID {}", shader_id);
                }
            }
            1 => {
                if let Some(D3D11Resource::PixelShader { shader }) = self.resources.get(&shader_id)
                {
                    unsafe {
                        self.context.PSSetShader(shader, None);
                    }
                } else {
                    warn!("SetShader: Invalid pixel shader ID {}", shader_id);
                }
            }
            _ => {
                warn!("SetShader: Unimplemented stage {}", stage);
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
            self.context.DrawInstanced(
                vertex_count,
                instance_count,
                start_vertex,
                start_instance,
            );
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
        if let Some(D3D11Resource::DepthStencilView { dsv }) = self.resources.get(&dsv_id) {
            debug!(
                "ClearDepthStencil: dsv={}, flags={}, depth={}, stencil={}",
                dsv_id, clear_flags, depth, stencil
            );
            unsafe {
                let flags = if clear_flags & 1 != 0 {
                    D3D11_CLEAR_DEPTH
                } else {
                    Default::default()
                } | if clear_flags & 2 != 0 {
                    D3D11_CLEAR_STENCIL
                } else {
                    Default::default()
                };
                self.context.ClearDepthStencilView(dsv, flags, depth, stencil);
            }
        } else {
            warn!("ClearDepthStencil: Invalid DSV ID {}", dsv_id);
        }
    }

    /// Copy entire resource
    pub fn copy_resource(&mut self, dst_id: ResourceId, src_id: ResourceId) {
        let src_resource = match self.resources.get(&src_id) {
            Some(D3D11Resource::Texture2D { texture, .. }) => {
                Some(texture.clone().cast().ok())
            }
            Some(D3D11Resource::Buffer { buffer, .. }) => {
                Some(buffer.clone().cast().ok())
            }
            _ => None,
        };

        let dst_resource = match self.resources.get(&dst_id) {
            Some(D3D11Resource::Texture2D { texture, .. }) => {
                Some(texture.clone().cast().ok())
            }
            Some(D3D11Resource::Buffer { buffer, .. }) => {
                Some(buffer.clone().cast().ok())
            }
            _ => None,
        };

        if let (Some(Some(dst)), Some(Some(src))) = (dst_resource, src_resource) {
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
}
