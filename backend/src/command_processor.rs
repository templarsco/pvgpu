//! Command Processor Module
//!
//! Reads commands from the ring buffer and dispatches to D3D11 renderer.

use crate::d3d11::D3D11Renderer;
use crate::protocol::*;
use anyhow::Result;
use tracing::{debug, warn};
use windows::Win32::Foundation::RECT;
use windows::Win32::Graphics::Direct3D11::D3D11_VIEWPORT;
use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT;

/// Processes commands from the shared memory ring buffer.
pub struct CommandProcessor {
    renderer: D3D11Renderer,
    current_fence: u64,
    /// Last present command info (backbuffer_id, sync_interval)
    pending_present: Option<(u32, u32)>,
}

impl CommandProcessor {
    pub fn new(renderer: D3D11Renderer) -> Self {
        Self {
            renderer,
            current_fence: 0,
            pending_present: None,
        }
    }

    /// Process a single command from the ring buffer.
    /// Returns the number of bytes consumed.
    pub fn process_command(&mut self, data: &[u8]) -> Result<usize> {
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
            PVGPU_CMD_CREATE_RESOURCE => self.handle_create_resource(cmd_data)?,
            PVGPU_CMD_DESTROY_RESOURCE => self.handle_destroy_resource(&header)?,
            PVGPU_CMD_COPY_RESOURCE => self.handle_copy_resource(cmd_data)?,
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
            _ => {
                warn!("Unknown command type: 0x{:04X}", header.command_type);
            }
        }

        Ok(header.command_size as usize)
    }

    fn handle_create_resource(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdCreateResource =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdCreateResource) };

        debug!(
            "CreateResource: id={}, type={}, {}x{}x{}, format={}",
            cmd.header.resource_id, cmd.resource_type, cmd.width, cmd.height, cmd.depth, cmd.format
        );

        let resource_id = cmd.header.resource_id;

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
                    None, // TODO: Get initial data from heap
                )?;
            }
            // Buffer
            4 => {
                self.renderer.create_buffer(
                    resource_id,
                    cmd.width, // For buffers, width is the size
                    cmd.bind_flags,
                    None, // TODO: Get initial data from heap
                )?;
            }
            // VertexShader
            5 => {
                // Bytecode should be at heap_offset in shared memory
                // For now, we can't access it directly - need shared memory reference
                warn!("VertexShader creation requires shared memory access - not implemented");
            }
            // PixelShader
            6 => {
                warn!("PixelShader creation requires shared memory access - not implemented");
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

        // Flush to ensure all prior commands are complete
        self.renderer.flush();

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

        self.renderer
            .set_vertex_buffer(cmd.slot, cmd.buffer_id, cmd.stride, cmd.offset);
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
        let cmd: CmdSetSampler =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetSampler) };

        self.renderer
            .set_sampler(cmd.stage, cmd.slot, cmd.sampler_id);
        Ok(())
    }

    fn handle_set_shader_resource(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetShaderResource =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetShaderResource) };

        self.renderer
            .set_shader_resource(cmd.stage, cmd.slot, cmd.srv_id);
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
}
