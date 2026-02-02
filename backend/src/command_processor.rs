//! Command Processor Module
//!
//! Reads commands from the ring buffer and dispatches to D3D11 renderer.

use crate::d3d11::D3D11Renderer;
use crate::protocol::*;
use anyhow::Result;
use tracing::{debug, warn};

/// Processes commands from the shared memory ring buffer.
pub struct CommandProcessor {
    renderer: D3D11Renderer,
    current_fence: u64,
}

impl CommandProcessor {
    pub fn new(renderer: D3D11Renderer) -> Self {
        Self {
            renderer,
            current_fence: 0,
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
            PVGPU_CMD_CREATE_RESOURCE => self.handle_create_resource(cmd_data)?,
            PVGPU_CMD_DESTROY_RESOURCE => self.handle_destroy_resource(&header)?,
            PVGPU_CMD_DRAW => self.handle_draw(cmd_data)?,
            PVGPU_CMD_DRAW_INDEXED => self.handle_draw_indexed(cmd_data)?,
            PVGPU_CMD_FENCE => self.handle_fence(cmd_data)?,
            PVGPU_CMD_PRESENT => self.handle_present(cmd_data)?,
            PVGPU_CMD_CLEAR_RENDER_TARGET => self.handle_clear_render_target(cmd_data)?,
            PVGPU_CMD_SET_VIEWPORT => self.handle_set_viewport(cmd_data)?,
            PVGPU_CMD_SET_SHADER => self.handle_set_shader(cmd_data)?,
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
            "CreateResource: type={}, {}x{}",
            cmd.resource_type, cmd.width, cmd.height
        );
        // TODO: Create actual D3D11 resource
        Ok(())
    }

    fn handle_destroy_resource(&mut self, header: &CommandHeader) -> Result<()> {
        debug!("DestroyResource: id={}", header.resource_id);
        self.renderer.destroy_resource(header.resource_id);
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

    fn handle_fence(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdFence = unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdFence) };
        self.current_fence = cmd.fence_value;
        debug!("Fence: value={}", cmd.fence_value);
        // TODO: Signal fence to guest via IRQ
        Ok(())
    }

    fn handle_present(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdPresent =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdPresent) };
        self.renderer.present(cmd.backbuffer_id, cmd.sync_interval);
        Ok(())
    }

    fn handle_clear_render_target(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdClearRenderTarget =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdClearRenderTarget) };
        debug!(
            "ClearRenderTarget: rtv={}, color={:?}",
            cmd.rtv_id, cmd.color
        );
        // TODO: Call ID3D11DeviceContext::ClearRenderTargetView
        Ok(())
    }

    fn handle_set_viewport(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetViewport =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetViewport) };
        debug!("SetViewport: {} viewports", cmd.num_viewports);
        // TODO: Call ID3D11DeviceContext::RSSetViewports
        Ok(())
    }

    fn handle_set_shader(&mut self, data: &[u8]) -> Result<()> {
        let cmd: CmdSetShader =
            unsafe { std::ptr::read_unaligned(data.as_ptr() as *const CmdSetShader) };
        debug!("SetShader: stage={}, id={}", cmd.stage, cmd.shader_id);
        // TODO: Call appropriate XXSetShader based on stage
        Ok(())
    }

    /// Get the current fence value.
    pub fn current_fence(&self) -> u64 {
        self.current_fence
    }
}
