//! Command Processor Module
//!
//! Reads commands from the ring buffer and dispatches to D3D11 renderer.

use crate::d3d11::{D3D11Renderer, MapResult, UpdateBox};
use crate::protocol::*;
use anyhow::Result;
use std::collections::HashMap;
use tracing::{debug, info, warn};
use windows::Win32::Foundation::RECT;
use windows::Win32::Graphics::Direct3D11::D3D11_VIEWPORT;
use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT;

/// Processes commands from the shared memory ring buffer.
pub struct CommandProcessor {
    renderer: D3D11Renderer,
    current_fence: u64,
    /// Last present command info (backbuffer_id, sync_interval)
    pending_present: Option<(u32, u32)>,
    /// Pending resize request (width, height)
    pending_resize: Option<(u32, u32)>,
    /// Active map operations: (resource_id, subresource) -> MapResult
    active_maps: HashMap<(u32, u32), MapResult>,
    /// Statistics tracking
    stats: CommandProcessorStats,
}

/// Statistics for command processing
#[derive(Default, Debug)]
pub struct CommandProcessorStats {
    pub commands_processed: u64,
    pub draw_calls: u64,
    pub presents: u64,
    pub resources_created: u64,
    pub resources_destroyed: u64,
    pub errors: u64,
}

impl CommandProcessor {
    pub fn new(renderer: D3D11Renderer) -> Self {
        Self {
            renderer,
            current_fence: 0,
            pending_present: None,
            pending_resize: None,
            active_maps: HashMap::new(),
            stats: CommandProcessorStats::default(),
        }
    }

    /// Process a single command from the ring buffer.
    /// Returns the number of bytes consumed.
    /// `heap` is the shared memory heap for data transfer operations.
    pub fn process_command(&mut self, data: &[u8], heap: &[u8]) -> Result<usize> {
        if data.len() < PVGPU_CMD_HEADER_SIZE {
            return Err(anyhow::anyhow!("Command too small"));
        }

        // Parse header
        let header: CommandHeader =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CommandHeader) };

        if header.command_size as usize > data.len() {
            return Err(anyhow::anyhow!("Command size exceeds available data"));
        }

        let cmd_data = &data[..header.command_size as usize];

        match header.command_type {
            // Resource commands
            PVGPU_CMD_CREATE_RESOURCE => self.handle_create_resource(cmd_data, heap)?,
            PVGPU_CMD_DESTROY_RESOURCE => self.handle_destroy_resource(&header)?,
            PVGPU_CMD_OPEN_RESOURCE => self.handle_open_resource(cmd_data, heap)?,
            PVGPU_CMD_COPY_RESOURCE => self.handle_copy_resource(cmd_data)?,
            PVGPU_CMD_CREATE_SHADER => self.handle_create_shader(cmd_data, heap)?,
            PVGPU_CMD_DESTROY_SHADER => self.handle_destroy_shader(cmd_data)?,
            PVGPU_CMD_MAP_RESOURCE => self.handle_map_resource(cmd_data, heap)?,
            PVGPU_CMD_UNMAP_RESOURCE => self.handle_unmap_resource(cmd_data, heap)?,
            PVGPU_CMD_UPDATE_RESOURCE => self.handle_update_resource(cmd_data, heap)?,
            // State commands
            PVGPU_CMD_SET_RENDER_TARGET => self.handle_set_render_target(cmd_data)?,
            PVGPU_CMD_SET_VIEWPORT => self.handle_set_viewport(cmd_data)?,
            PVGPU_CMD_SET_SCISSOR => self.handle_set_scissor(cmd_data)?,
            PVGPU_CMD_SET_BLEND_STATE => self.handle_set_blend_state(cmd_data)?,
            PVGPU_CMD_SET_RASTERIZER_STATE => self.handle_set_rasterizer_state(cmd_data)?,
            PVGPU_CMD_SET_DEPTH_STENCIL => self.handle_set_depth_stencil(cmd_data)?,
            PVGPU_CMD_SET_SHADER => self.handle_set_shader(cmd_data)?,
            PVGPU_CMD_SET_SAMPLER => self.handle_set_sampler(cmd_data)?,
            PVGPU_CMD_SET_CONSTANT_BUFFER => self.handle_set_constant_buffer(cmd_data)?,
            PVGPU_CMD_SET_VERTEX_BUFFER => self.handle_set_vertex_buffer(cmd_data)?,
            PVGPU_CMD_SET_INDEX_BUFFER => self.handle_set_index_buffer(cmd_data)?,
            PVGPU_CMD_SET_INPUT_LAYOUT => self.handle_set_input_layout(cmd_data)?,
            PVGPU_CMD_SET_PRIMITIVE_TOPOLOGY => self.handle_set_primitive_topology(cmd_data)?,
            PVGPU_CMD_SET_SHADER_RESOURCE => self.handle_set_shader_resource(cmd_data)?,
            // Draw commands
            PVGPU_CMD_DRAW => self.handle_draw(cmd_data)?,
            PVGPU_CMD_DRAW_INDEXED => self.handle_draw_indexed(cmd_data)?,
            PVGPU_CMD_DRAW_INSTANCED => self.handle_draw_instanced(cmd_data)?,
            PVGPU_CMD_DRAW_INDEXED_INSTANCED => self.handle_draw_indexed_instanced(cmd_data)?,
            PVGPU_CMD_DISPATCH => self.handle_dispatch(cmd_data)?,
            PVGPU_CMD_CLEAR_RENDER_TARGET => self.handle_clear_render_target(cmd_data)?,
            PVGPU_CMD_CLEAR_DEPTH_STENCIL => self.handle_clear_depth_stencil(cmd_data)?,
            // Sync commands
            PVGPU_CMD_FENCE => self.handle_fence(cmd_data)?,
            PVGPU_CMD_PRESENT => self.handle_present(cmd_data)?,
            PVGPU_CMD_FLUSH => self.handle_flush()?,
            PVGPU_CMD_RESIZE_BUFFERS => self.handle_resize_buffers(cmd_data)?,
            _ => {
                warn!("Unknown command type: 0x{:04X}", header.command_type);
            }
        }

        // Track statistics based on command type
        self.stats.commands_processed += 1;
        match header.command_type {
            PVGPU_CMD_CREATE_RESOURCE => self.stats.resources_created += 1,
            PVGPU_CMD_DESTROY_RESOURCE => self.stats.resources_destroyed += 1,
            PVGPU_CMD_DRAW
            | PVGPU_CMD_DRAW_INDEXED
            | PVGPU_CMD_DRAW_INSTANCED
            | PVGPU_CMD_DRAW_INDEXED_INSTANCED
            | PVGPU_CMD_DISPATCH => self.stats.draw_calls += 1,
            PVGPU_CMD_PRESENT => self.stats.presents += 1,
            _ => {}
        }

        Ok(header.command_size as usize)
    }

    fn handle_create_resource(&mut self, data: &[u8], heap: &[u8]) -> Result<()> {
        let cmd: CmdCreateResource =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdCreateResource) };

        debug!(
            "CreateResource: id={}, type={}, {}x{}x{}, format={}, heap_offset={}, data_size={}",
            cmd.header.resource_id,
            cmd.resource_type,
            cmd.width,
            cmd.height,
            cmd.depth,
            cmd.format,
            cmd.heap_offset,
            cmd.data_size
        );

        let resource_id = cmd.header.resource_id;

        // Get initial data from heap if provided
        let initial_data = if cmd.data_size > 0 && cmd.heap_offset > 0 {
            let offset = cmd.heap_offset as usize;
            let size = cmd.data_size as usize;
            if offset + size <= heap.len() {
                Some(&heap[offset..offset + size])
            } else {
                warn!("CreateResource: heap_offset + data_size exceeds heap bounds");
                None
            }
        } else {
            None
        };

        match cmd.resource_type {
            // Texture2D
            2 => {
                let format = DXGI_FORMAT(cmd.format as i32);
                self.renderer.create_texture2d(
                    resource_id,
                    cmd.width,
                    cmd.height,
                    format,
                    cmd.bind_flags,
                    initial_data,
                )?;
            }
            // Buffer
            4 => {
                self.renderer.create_buffer(
                    resource_id,
                    cmd.width, // For buffers, width is the size
                    cmd.bind_flags,
                    initial_data,
                )?;
            }
            // VertexShader
            5 => {
                if let Some(bytecode) = initial_data {
                    if let Err(e) = self.renderer.create_vertex_shader(resource_id, bytecode) {
                        warn!("VertexShader creation failed for id={}: {}", resource_id, e);
                        // Return shader compile error - the command is consumed but failed
                        return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                    }
                } else {
                    warn!("VertexShader creation requires bytecode in heap");
                    return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                }
            }
            // PixelShader
            6 => {
                if let Some(bytecode) = initial_data {
                    if let Err(e) = self.renderer.create_pixel_shader(resource_id, bytecode) {
                        warn!("PixelShader creation failed for id={}: {}", resource_id, e);
                        // Return shader compile error - the command is consumed but failed
                        return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                    }
                } else {
                    warn!("PixelShader creation requires bytecode in heap");
                    return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                }
            }
            // GeometryShader
            7 => {
                if let Some(bytecode) = initial_data {
                    if let Err(e) = self.renderer.create_geometry_shader(resource_id, bytecode) {
                        warn!(
                            "GeometryShader creation failed for id={}: {}",
                            resource_id, e
                        );
                        return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                    }
                } else {
                    warn!("GeometryShader creation requires bytecode in heap");
                    return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                }
            }
            // HullShader
            8 => {
                if let Some(bytecode) = initial_data {
                    if let Err(e) = self.renderer.create_hull_shader(resource_id, bytecode) {
                        warn!("HullShader creation failed for id={}: {}", resource_id, e);
                        return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                    }
                } else {
                    warn!("HullShader creation requires bytecode in heap");
                    return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                }
            }
            // DomainShader
            9 => {
                if let Some(bytecode) = initial_data {
                    if let Err(e) = self.renderer.create_domain_shader(resource_id, bytecode) {
                        warn!("DomainShader creation failed for id={}: {}", resource_id, e);
                        return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                    }
                } else {
                    warn!("DomainShader creation requires bytecode in heap");
                    return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                }
            }
            // ComputeShader
            10 => {
                if let Some(bytecode) = initial_data {
                    if let Err(e) = self.renderer.create_compute_shader(resource_id, bytecode) {
                        warn!(
                            "ComputeShader creation failed for id={}: {}",
                            resource_id, e
                        );
                        return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                    }
                } else {
                    warn!("ComputeShader creation requires bytecode in heap");
                    return Err(anyhow::anyhow!("SHADER_COMPILE:{}", resource_id));
                }
            }
            _ => {
                warn!("Unknown resource type: {}", cmd.resource_type);
            }
        }

        Ok(())
    }

    fn handle_destroy_resource(&mut self, header: &CommandHeader) -> Result<()> {
        debug!("DestroyResource: id={}", header.resource_id);
        self.renderer.destroy_resource(header.resource_id);
        Ok(())
    }

    fn handle_open_resource(&mut self, data: &[u8], _heap: &[u8]) -> Result<()> {
        let cmd: CmdOpenResource =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdOpenResource) };

        debug!(
            "OpenResource: new_id={}, shared_handle={}, type={}",
            cmd.header.resource_id, cmd.shared_handle, cmd.resource_type
        );

        let new_id = cmd.header.resource_id;
        let original_id = cmd.shared_handle;

        // For shared resources, we create an alias to the original resource
        // The backend maintains resource ownership - the "open" creates a reference
        // that maps new_id -> same underlying D3D11 resource as original_id
        if self.renderer.get_resource(original_id).is_some() {
            // Clone the resource reference - both IDs point to the same D3D11 object
            // D3D11 COM objects are refcounted, so this is safe
            match cmd.resource_type {
                // Texture2D
                2 => {
                    if let Some(tex) = self.renderer.get_texture(original_id) {
                        // AddRef the texture and register under new ID
                        unsafe {
                            use windows::core::Interface;
                            let raw: *mut std::ffi::c_void = tex.as_raw();
                            let tex_clone: windows::Win32::Graphics::Direct3D11::ID3D11Texture2D =
                                windows::Win32::Graphics::Direct3D11::ID3D11Texture2D::from_raw(
                                    raw,
                                );
                            // AddRef by cloning
                            let tex_ref = tex_clone.clone();
                            std::mem::forget(tex_clone); // Don't release the original
                            self.renderer.register_texture(new_id, tex_ref);
                        }
                    } else {
                        warn!("OpenResource: original {} is not a texture", original_id);
                    }
                }
                // Buffer
                4 => {
                    if let Some(buf) = self.renderer.get_buffer(original_id) {
                        unsafe {
                            use windows::core::Interface;
                            let raw: *mut std::ffi::c_void = buf.as_raw();
                            let buf_clone: windows::Win32::Graphics::Direct3D11::ID3D11Buffer =
                                windows::Win32::Graphics::Direct3D11::ID3D11Buffer::from_raw(raw);
                            let buf_ref = buf_clone.clone();
                            std::mem::forget(buf_clone);
                            self.renderer.register_buffer(new_id, buf_ref);
                        }
                    } else {
                        warn!("OpenResource: original {} is not a buffer", original_id);
                    }
                }
                _ => {
                    warn!(
                        "OpenResource: unsupported resource type {} for sharing",
                        cmd.resource_type
                    );
                }
            }
        } else {
            warn!("OpenResource: original resource {} not found", original_id);
        }

        Ok(())
    }

    fn handle_map_resource(&mut self, data: &[u8], heap: &[u8]) -> Result<()> {
        let cmd: CmdMapResource =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdMapResource) };

        debug!(
            "MapResource: id={}, subresource={}, type={}, heap_offset={}",
            cmd.resource_id, cmd.subresource, cmd.map_type, cmd.heap_offset
        );

        // Map the resource
        let map_result =
            self.renderer
                .map_resource(cmd.resource_id, cmd.subresource, cmd.map_type)?;

        // For read maps, copy GPU data to shared memory heap
        if cmd.map_type == 1 || cmd.map_type == 3 {
            // Read or ReadWrite
            let offset = cmd.heap_offset as usize;
            let size = std::cmp::min(map_result.size, heap.len().saturating_sub(offset));
            if size > 0 && !map_result.data_ptr.is_null() {
                // Note: We need mutable heap access here. The caller must provide this.
                // For now, we store the map result for later unmap which will handle the copy.
                debug!(
                    "MapResource: read map, data will be available at heap offset {}",
                    offset
                );
            }
        }

        // Store the map result for later unmap
        let key = (cmd.resource_id, cmd.subresource);
        self.active_maps.insert(key, map_result);

        Ok(())
    }

    fn handle_unmap_resource(&mut self, data: &[u8], heap: &[u8]) -> Result<()> {
        let cmd: CmdUnmapResource =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdUnmapResource) };

        debug!(
            "UnmapResource: id={}, subresource={}, heap_offset={}, data_size={}",
            cmd.resource_id, cmd.subresource, cmd.heap_offset, cmd.data_size
        );

        let key = (cmd.resource_id, cmd.subresource);

        if let Some(map_result) = self.active_maps.remove(&key) {
            // For write operations, copy data from heap to the mapped buffer first
            if cmd.data_size > 0 && !map_result.data_ptr.is_null() {
                let offset = cmd.heap_offset as usize;
                let size = cmd.data_size as usize;
                if offset + size <= heap.len() {
                    unsafe {
                        std::ptr::copy_nonoverlapping(
                            heap[offset..].as_ptr(),
                            map_result.data_ptr,
                            size,
                        );
                    }
                    debug!("UnmapResource: copied {} bytes from heap to staging", size);
                }
            }

            // Determine if this was a write operation
            let was_write = cmd.data_size > 0;

            self.renderer
                .unmap_resource(&map_result, cmd.subresource, was_write);
        } else {
            warn!(
                "UnmapResource: no active map for resource {} subresource {}",
                cmd.resource_id, cmd.subresource
            );
        }

        Ok(())
    }

    fn handle_update_resource(&mut self, data: &[u8], heap: &[u8]) -> Result<()> {
        let cmd: CmdUpdateResource =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdUpdateResource) };

        debug!(
            "UpdateResource: id={}, subresource={}, heap_offset={}, size={}, dst=({},{},{}), dim={}x{}x{}",
            cmd.resource_id, cmd.subresource, cmd.heap_offset, cmd.data_size,
            cmd.dst_x, cmd.dst_y, cmd.dst_z, cmd.width, cmd.height, cmd.depth
        );

        // Get data from heap
        let offset = cmd.heap_offset as usize;
        let size = cmd.data_size as usize;

        if offset + size > heap.len() {
            return Err(anyhow::anyhow!(
                "UpdateResource: heap_offset + data_size exceeds heap bounds"
            ));
        }

        let src_data = &heap[offset..offset + size];

        // Build destination box if non-zero dimensions specified
        let dst_box = if cmd.width > 0 || cmd.height > 0 || cmd.depth > 0 {
            Some(UpdateBox {
                left: cmd.dst_x,
                top: cmd.dst_y,
                front: cmd.dst_z,
                right: cmd.dst_x + cmd.width,
                bottom: cmd.dst_y + cmd.height,
                back: cmd.dst_z + cmd.depth,
            })
        } else {
            None
        };

        self.renderer.update_subresource(
            cmd.resource_id,
            cmd.subresource,
            src_data,
            dst_box,
            cmd.row_pitch,
            cmd.depth_pitch,
        )?;

        Ok(())
    }

    fn handle_set_render_target(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetRenderTarget =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetRenderTarget) };

        debug!(
            "SetRenderTarget: num_rtvs={}, dsv_id={}",
            cmd.num_rtvs, cmd.dsv_id
        );

        let rtv_ids: Vec<u32> = cmd.rtv_ids[..cmd.num_rtvs as usize].to_vec();
        let dsv_id = if cmd.dsv_id == 0 {
            None
        } else {
            Some(cmd.dsv_id)
        };

        self.renderer.set_render_targets(&rtv_ids, dsv_id)?;
        Ok(())
    }

    fn handle_set_viewport(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetViewport =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetViewport) };

        debug!("SetViewport: {} viewports", cmd.num_viewports);

        let viewports: Vec<D3D11_VIEWPORT> = cmd.viewports[..cmd.num_viewports as usize]
            .iter()
            .map(|v| D3D11_VIEWPORT {
                TopLeftX: v.x,
                TopLeftY: v.y,
                Width: v.width,
                Height: v.height,
                MinDepth: v.min_depth,
                MaxDepth: v.max_depth,
            })
            .collect();

        self.renderer.set_viewports(&viewports);
        Ok(())
    }

    fn handle_draw(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdDraw = unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdDraw) };
        self.renderer.draw(cmd.vertex_count, cmd.start_vertex);
        Ok(())
    }

    fn handle_draw_indexed(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdDrawIndexed =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdDrawIndexed) };
        self.renderer
            .draw_indexed(cmd.index_count, cmd.start_index, cmd.base_vertex);
        Ok(())
    }

    fn handle_clear_render_target(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdClearRenderTarget =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdClearRenderTarget) };

        debug!(
            "ClearRenderTarget: rtv={}, color={:?}",
            cmd.rtv_id, cmd.color
        );

        self.renderer.clear_render_target(cmd.rtv_id, &cmd.color);
        Ok(())
    }

    fn handle_fence(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdFence = unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdFence) };
        self.current_fence = cmd.fence_value;

        debug!("Fence: value={}", cmd.fence_value);

        // Note: We intentionally do NOT flush here. D3D11 guarantees in-order
        // execution, so all prior commands are already queued. Flushing on every
        // fence destroys GPU pipelining. The guest should use WaitFence if it
        // needs to ensure completion before proceeding.

        Ok(())
    }

    fn handle_present(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdPresent =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdPresent) };

        debug!(
            "Present: backbuffer={}, sync_interval={}",
            cmd.backbuffer_id, cmd.sync_interval
        );

        // Store the present request - the main loop will handle actual presentation
        self.pending_present = Some((cmd.backbuffer_id, cmd.sync_interval));

        // Flush to ensure all prior rendering is complete
        self.renderer.flush();
        Ok(())
    }

    fn handle_flush(&mut self) -> Result<()> {
        debug!("Flush");
        self.renderer.flush();
        Ok(())
    }

    fn handle_set_shader(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetShader =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetShader) };

        debug!("SetShader: stage={}, id={}", cmd.stage, cmd.shader_id);

        self.renderer.set_shader(cmd.stage, cmd.shader_id);
        Ok(())
    }

    fn handle_set_vertex_buffer(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetVertexBuffer =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetVertexBuffer) };

        let count = (cmd.num_buffers as usize).min(16);
        for i in 0..count {
            let binding = &cmd.buffers[i];
            self.renderer.set_vertex_buffer(
                cmd.start_slot + i as u32,
                binding.buffer_id,
                binding.stride,
                binding.offset,
            );
        }
        Ok(())
    }

    fn handle_set_index_buffer(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetIndexBuffer =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetIndexBuffer) };

        let format = DXGI_FORMAT(cmd.format as i32);
        self.renderer
            .set_index_buffer(cmd.buffer_id, format, cmd.offset);
        Ok(())
    }

    fn handle_set_constant_buffer(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetConstantBuffer =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetConstantBuffer) };

        self.renderer
            .set_constant_buffer(cmd.stage, cmd.slot, cmd.buffer_id);
        Ok(())
    }

    fn handle_set_input_layout(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetInputLayout =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetInputLayout) };

        self.renderer.set_input_layout(cmd.layout_id);
        Ok(())
    }

    fn handle_set_primitive_topology(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetPrimitiveTopology =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetPrimitiveTopology) };

        self.renderer.set_primitive_topology(cmd.topology);
        Ok(())
    }

    fn handle_set_sampler(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetSamplers =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetSamplers) };

        let count = (cmd.num_samplers as usize).min(16);
        for i in 0..count {
            self.renderer
                .set_sampler(cmd.stage, cmd.start_slot + i as u32, cmd.sampler_ids[i]);
        }
        Ok(())
    }

    fn handle_set_shader_resource(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetShaderResources =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetShaderResources) };

        let count = (cmd.num_views as usize).min(128);
        for i in 0..count {
            self.renderer.set_shader_resource(
                cmd.stage,
                cmd.start_slot + i as u32,
                cmd.view_ids[i],
            );
        }
        Ok(())
    }

    fn handle_set_blend_state(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetBlendState =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetBlendState) };

        self.renderer
            .set_blend_state(cmd.state_id, &cmd.blend_factor, cmd.sample_mask);
        Ok(())
    }

    fn handle_set_rasterizer_state(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetRasterizerState =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetRasterizerState) };

        self.renderer.set_rasterizer_state(cmd.state_id);
        Ok(())
    }

    fn handle_set_depth_stencil(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetDepthStencil =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetDepthStencil) };

        self.renderer
            .set_depth_stencil_state(cmd.state_id, cmd.stencil_ref);
        Ok(())
    }

    fn handle_set_scissor(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetScissor =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetScissor) };

        let rects: Vec<RECT> = cmd.rects[..cmd.num_rects as usize]
            .iter()
            .map(|r| RECT {
                left: r.left,
                top: r.top,
                right: r.right,
                bottom: r.bottom,
            })
            .collect();

        self.renderer.set_scissor_rects(&rects);
        Ok(())
    }

    fn handle_draw_instanced(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdDrawInstanced =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdDrawInstanced) };

        self.renderer.draw_instanced(
            cmd.vertex_count,
            cmd.instance_count,
            cmd.start_vertex,
            cmd.start_instance,
        );
        Ok(())
    }

    fn handle_draw_indexed_instanced(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdDrawIndexedInstanced =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdDrawIndexedInstanced) };

        self.renderer.draw_indexed_instanced(
            cmd.index_count,
            cmd.instance_count,
            cmd.start_index,
            cmd.base_vertex,
            cmd.start_instance,
        );
        Ok(())
    }

    fn handle_dispatch(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdDispatch =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdDispatch) };

        self.renderer.dispatch(
            cmd.thread_group_count_x,
            cmd.thread_group_count_y,
            cmd.thread_group_count_z,
        );
        Ok(())
    }

    fn handle_clear_depth_stencil(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdClearDepthStencil =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdClearDepthStencil) };

        self.renderer
            .clear_depth_stencil(cmd.dsv_id, cmd.clear_flags, cmd.depth, cmd.stencil);
        Ok(())
    }

    fn handle_copy_resource(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdCopyResource =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdCopyResource) };

        self.renderer
            .copy_resource(cmd.dst_resource_id, cmd.src_resource_id);
        Ok(())
    }

    fn handle_create_shader(&mut self, data: &[u8], heap: &[u8]) -> Result<()> {
        let cmd: CmdCreateShader =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdCreateShader) };

        debug!(
            "CreateShader: id={}, type={}, bytecode_size={}, bytecode_offset={}",
            cmd.shader_id, cmd.shader_type, cmd.bytecode_size, cmd.bytecode_offset
        );

        let shader_id = cmd.shader_id;

        let offset = cmd.bytecode_offset as usize;
        let size = cmd.bytecode_size as usize;

        if size == 0 {
            warn!("CreateShader: zero bytecode size");
            return Ok(());
        }

        if offset + size > heap.len() {
            return Err(anyhow::anyhow!(
                "CreateShader: bytecode_offset + bytecode_size exceeds heap bounds"
            ));
        }

        let bytecode = &heap[offset..offset + size];

        match cmd.shader_type {
            0 => {
                self.renderer.create_vertex_shader(shader_id, bytecode)?;
            }
            1 => {
                self.renderer.create_pixel_shader(shader_id, bytecode)?;
            }
            2 => {
                self.renderer.create_geometry_shader(shader_id, bytecode)?;
            }
            3 => {
                self.renderer.create_hull_shader(shader_id, bytecode)?;
            }
            4 => {
                self.renderer.create_domain_shader(shader_id, bytecode)?;
            }
            5 => {
                self.renderer.create_compute_shader(shader_id, bytecode)?;
            }
            _ => {
                warn!("CreateShader: unknown shader type {}", cmd.shader_type);
            }
        }

        Ok(())
    }

    fn handle_destroy_shader(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdDestroyShader =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdDestroyShader) };

        debug!("DestroyShader: id={}", cmd.shader_id);
        self.renderer.destroy_resource(cmd.shader_id);
        Ok(())
    }

    /// Get the current fence value.
    pub fn current_fence(&self) -> u64 {
        self.current_fence
    }

    /// Get a reference to the renderer
    pub fn renderer(&self) -> &D3D11Renderer {
        &self.renderer
    }

    /// Get a mutable reference to the renderer
    pub fn renderer_mut(&mut self) -> &mut D3D11Renderer {
        &mut self.renderer
    }

    /// Check if a present is pending
    pub fn has_pending_present(&self) -> bool {
        self.pending_present.is_some()
    }

    /// Take the pending present info (backbuffer_id, sync_interval)
    /// Returns None if no present is pending
    pub fn take_pending_present(&mut self) -> Option<(u32, u32)> {
        self.pending_present.take()
    }

    /// Check if a resize is pending
    pub fn has_pending_resize(&self) -> bool {
        self.pending_resize.is_some()
    }

    /// Take the pending resize info (width, height)
    /// Returns None if no resize is pending
    pub fn take_pending_resize(&mut self) -> Option<(u32, u32)> {
        self.pending_resize.take()
    }

    fn handle_resize_buffers(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdResizeBuffers =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdResizeBuffers) };

        debug!(
            "ResizeBuffers: swapchain={}, {}x{}, format={}, buffer_count={}, flags={}",
            cmd.swapchain_id, cmd.width, cmd.height, cmd.format, cmd.buffer_count, cmd.flags
        );

        // Store the resize request - the main loop will handle actual resize
        // We only support the default swapchain (id=0) for now
        if cmd.swapchain_id == 0 && cmd.width > 0 && cmd.height > 0 {
            self.pending_resize = Some((cmd.width, cmd.height));
        }

        // Flush to ensure all prior rendering is complete before resize
        self.renderer.flush();
        Ok(())
    }

    /// Get a reference to the processing statistics
    pub fn stats(&self) -> &CommandProcessorStats {
        &self.stats
    }

    /// Log and reset statistics
    pub fn log_and_reset_stats(&mut self) {
        info!(
            "CommandProcessor stats: commands={}, draws={}, presents={}, resources_created={}, resources_destroyed={}, errors={}",
            self.stats.commands_processed,
            self.stats.draw_calls,
            self.stats.presents,
            self.stats.resources_created,
            self.stats.resources_destroyed,
            self.stats.errors
        );
        self.stats = CommandProcessorStats::default();
    }

    /// Increment error counter
    fn record_error(&mut self) {
        self.stats.errors += 1;
    }
}
