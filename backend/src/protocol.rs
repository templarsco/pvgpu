//! PVGPU Protocol - Rust bindings
//!
//! Manual Rust bindings for the PVGPU protocol defined in pvgpu_protocol.h.
//! These match the C structures for shared memory communication.

use std::sync::atomic::{AtomicU64, Ordering};

/// Magic number: "PVGP" in little-endian
pub const PVGPU_MAGIC: u32 = 0x50564750;
pub const PVGPU_VERSION_MAJOR: u32 = 1;
pub const PVGPU_VERSION_MINOR: u32 = 0;
pub const PVGPU_VERSION: u32 = (PVGPU_VERSION_MAJOR << 16) | PVGPU_VERSION_MINOR;

/// Default sizes
pub const PVGPU_CONTROL_REGION_SIZE: usize = 0x1000; // 4KB
pub const PVGPU_COMMAND_RING_SIZE: usize = 0x1000000; // 16MB
pub const PVGPU_DEFAULT_SHMEM_SIZE: usize = 0x10000000; // 256MB

// =============================================================================
// Feature Flags
// =============================================================================

pub const PVGPU_FEATURE_D3D11: u64 = 1 << 0;
pub const PVGPU_FEATURE_D3D12: u64 = 1 << 1;
pub const PVGPU_FEATURE_COMPUTE: u64 = 1 << 2;
pub const PVGPU_FEATURE_GEOMETRY: u64 = 1 << 3;
pub const PVGPU_FEATURE_TESSELLATION: u64 = 1 << 4;
pub const PVGPU_FEATURE_MSAA: u64 = 1 << 5;
pub const PVGPU_FEATURE_HDR: u64 = 1 << 6;
pub const PVGPU_FEATURE_VSYNC: u64 = 1 << 7;
pub const PVGPU_FEATURE_TRIPLE_BUFFER: u64 = 1 << 8;

pub const PVGPU_FEATURES_MVP: u64 =
    PVGPU_FEATURE_D3D11 | PVGPU_FEATURE_COMPUTE | PVGPU_FEATURE_VSYNC;

// =============================================================================
// Command Types
// =============================================================================

// Resource commands: 0x0001 - 0x00FF
pub const PVGPU_CMD_CREATE_RESOURCE: u32 = 0x0001;
pub const PVGPU_CMD_DESTROY_RESOURCE: u32 = 0x0002;
pub const PVGPU_CMD_MAP_RESOURCE: u32 = 0x0003;
pub const PVGPU_CMD_UNMAP_RESOURCE: u32 = 0x0004;
pub const PVGPU_CMD_UPDATE_RESOURCE: u32 = 0x0005;
pub const PVGPU_CMD_COPY_RESOURCE: u32 = 0x0006;

// State commands: 0x0100 - 0x01FF
pub const PVGPU_CMD_SET_RENDER_TARGET: u32 = 0x0101;
pub const PVGPU_CMD_SET_VIEWPORT: u32 = 0x0102;
pub const PVGPU_CMD_SET_SCISSOR: u32 = 0x0103;
pub const PVGPU_CMD_SET_BLEND_STATE: u32 = 0x0104;
pub const PVGPU_CMD_SET_RASTERIZER_STATE: u32 = 0x0105;
pub const PVGPU_CMD_SET_DEPTH_STENCIL: u32 = 0x0106;
pub const PVGPU_CMD_SET_SHADER: u32 = 0x0107;
pub const PVGPU_CMD_SET_SAMPLER: u32 = 0x0108;
pub const PVGPU_CMD_SET_CONSTANT_BUFFER: u32 = 0x0109;
pub const PVGPU_CMD_SET_VERTEX_BUFFER: u32 = 0x010A;
pub const PVGPU_CMD_SET_INDEX_BUFFER: u32 = 0x010B;
pub const PVGPU_CMD_SET_INPUT_LAYOUT: u32 = 0x010C;
pub const PVGPU_CMD_SET_PRIMITIVE_TOPOLOGY: u32 = 0x010D;
pub const PVGPU_CMD_SET_SHADER_RESOURCE: u32 = 0x010E;

// Draw commands: 0x0200 - 0x02FF
pub const PVGPU_CMD_DRAW: u32 = 0x0201;
pub const PVGPU_CMD_DRAW_INDEXED: u32 = 0x0202;
pub const PVGPU_CMD_DRAW_INSTANCED: u32 = 0x0203;
pub const PVGPU_CMD_DRAW_INDEXED_INSTANCED: u32 = 0x0204;
pub const PVGPU_CMD_DISPATCH: u32 = 0x0205;
pub const PVGPU_CMD_CLEAR_RENDER_TARGET: u32 = 0x0206;
pub const PVGPU_CMD_CLEAR_DEPTH_STENCIL: u32 = 0x0207;

// Sync commands: 0x0300 - 0x03FF
pub const PVGPU_CMD_FENCE: u32 = 0x0301;
pub const PVGPU_CMD_PRESENT: u32 = 0x0302;
pub const PVGPU_CMD_FLUSH: u32 = 0x0303;
pub const PVGPU_CMD_WAIT_FENCE: u32 = 0x0304;

// =============================================================================
// Error Codes
// =============================================================================

pub const PVGPU_ERROR_SUCCESS: u32 = 0x0000;
pub const PVGPU_ERROR_INVALID_COMMAND: u32 = 0x0001;
pub const PVGPU_ERROR_RESOURCE_NOT_FOUND: u32 = 0x0002;
pub const PVGPU_ERROR_OUT_OF_MEMORY: u32 = 0x0003;
pub const PVGPU_ERROR_SHADER_COMPILE: u32 = 0x0004;
pub const PVGPU_ERROR_DEVICE_LOST: u32 = 0x0005;
pub const PVGPU_ERROR_INVALID_PARAMETER: u32 = 0x0006;
pub const PVGPU_ERROR_UNSUPPORTED_FORMAT: u32 = 0x0007;
pub const PVGPU_ERROR_BACKEND_DISCONNECTED: u32 = 0x0008;
pub const PVGPU_ERROR_RING_FULL: u32 = 0x0009;
pub const PVGPU_ERROR_TIMEOUT: u32 = 0x000A;
pub const PVGPU_ERROR_UNKNOWN: u32 = 0xFFFF;

// =============================================================================
// Resource Types
// =============================================================================

#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[allow(dead_code)]
pub enum ResourceType {
    Texture1D = 1,
    Texture2D = 2,
    Texture3D = 3,
    Buffer = 4,
    VertexShader = 5,
    PixelShader = 6,
    GeometryShader = 7,
    HullShader = 8,
    DomainShader = 9,
    ComputeShader = 10,
    InputLayout = 11,
    BlendState = 12,
    RasterizerState = 13,
    DepthStencilState = 14,
    SamplerState = 15,
    RenderTargetView = 16,
    DepthStencilView = 17,
    ShaderResourceView = 18,
    UnorderedAccessView = 19,
}

#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[allow(dead_code)]
pub enum ShaderStage {
    Vertex = 0,
    Pixel = 1,
    Geometry = 2,
    Hull = 3,
    Domain = 4,
    Compute = 5,
}

// =============================================================================
// Control Region (matches C struct layout)
// =============================================================================

/// Control Region at offset 0 of shared memory.
///
/// SAFETY: This struct must match the exact memory layout of PvgpuControlRegion in C.
/// It is designed to be mapped directly to shared memory.
#[repr(C)]
pub struct ControlRegion {
    // 0x000
    pub magic: u32,
    pub version: u32,
    pub features: u64,

    // Ring configuration - 0x010
    pub ring_offset: u32,
    pub ring_size: u32,
    pub heap_offset: u32,
    pub heap_size: u32,

    // Producer-consumer pointers - 0x020
    // These are atomic in C, we use AtomicU64 for Rust
    producer_ptr_raw: u64,
    consumer_ptr_raw: u64,

    // Fence synchronization - 0x030
    guest_fence_request_raw: u64,
    host_fence_completed_raw: u64,

    // Status and error - 0x040
    pub status: u32,
    pub error_code: u32,
    pub error_data: u32,
    _reserved1: u32,

    // Display configuration - 0x050
    pub display_width: u32,
    pub display_height: u32,
    pub display_refresh: u32,
    pub display_format: u32,

    // Reserved - 0x060 to 0xFFF
    _reserved: [u8; 0xFA0],
}

impl ControlRegion {
    /// Validate the control region has correct magic and version.
    pub fn validate(&self) -> Result<(), &'static str> {
        if self.magic != PVGPU_MAGIC {
            return Err("Invalid magic number");
        }
        if (self.version >> 16) != PVGPU_VERSION_MAJOR {
            return Err("Incompatible protocol version");
        }
        Ok(())
    }

    /// Get producer pointer atomically.
    pub fn producer_ptr(&self) -> u64 {
        // SAFETY: The memory is properly aligned and we're doing atomic read
        unsafe {
            let ptr = &self.producer_ptr_raw as *const u64 as *const AtomicU64;
            (*ptr).load(Ordering::Acquire)
        }
    }

    /// Get consumer pointer atomically.
    pub fn consumer_ptr(&self) -> u64 {
        unsafe {
            let ptr = &self.consumer_ptr_raw as *const u64 as *const AtomicU64;
            (*ptr).load(Ordering::Acquire)
        }
    }

    /// Set consumer pointer atomically (called by host).
    pub fn set_consumer_ptr(&self, value: u64) {
        unsafe {
            let ptr = &self.consumer_ptr_raw as *const u64 as *const AtomicU64;
            (*ptr).store(value, Ordering::Release);
        }
    }

    /// Get host fence completed value.
    pub fn host_fence_completed(&self) -> u64 {
        unsafe {
            let ptr = &self.host_fence_completed_raw as *const u64 as *const AtomicU64;
            (*ptr).load(Ordering::Acquire)
        }
    }

    /// Set host fence completed value (called by host after completing work).
    pub fn set_host_fence_completed(&self, value: u64) {
        unsafe {
            let ptr = &self.host_fence_completed_raw as *const u64 as *const AtomicU64;
            (*ptr).store(value, Ordering::Release);
        }
    }

    /// Check if there are pending commands in the ring.
    pub fn has_pending_commands(&self) -> bool {
        self.producer_ptr() > self.consumer_ptr()
    }

    /// Get number of pending bytes in the ring.
    pub fn pending_bytes(&self) -> u64 {
        self.producer_ptr().saturating_sub(self.consumer_ptr())
    }
}

// Verify size at compile time
const _: () = assert!(std::mem::size_of::<ControlRegion>() == PVGPU_CONTROL_REGION_SIZE);

// =============================================================================
// Command Header
// =============================================================================

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CommandHeader {
    pub command_type: u32,
    pub command_size: u32,
    pub resource_id: u32,
    pub flags: u32,
}

pub const PVGPU_CMD_HEADER_SIZE: usize = std::mem::size_of::<CommandHeader>();

// Command flags
#[allow(dead_code)]
pub const PVGPU_CMD_FLAG_SYNC: u32 = 1 << 0;
#[allow(dead_code)]
pub const PVGPU_CMD_FLAG_NO_FENCE: u32 = 1 << 1;

// =============================================================================
// Command Payloads
// =============================================================================

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdCreateResource {
    pub header: CommandHeader,
    pub resource_type: u32,
    pub format: u32,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub mip_levels: u32,
    pub sample_count: u32,
    pub sample_quality: u32,
    pub bind_flags: u32,
    pub misc_flags: u32,
    pub heap_offset: u32,
    pub data_size: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
#[allow(dead_code)]
pub struct CmdSetRenderTarget {
    pub header: CommandHeader,
    pub num_rtvs: u32,
    pub dsv_id: u32,
    pub rtv_ids: [u32; 8],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct Viewport {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub min_depth: f32,
    pub max_depth: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetViewport {
    pub header: CommandHeader,
    pub num_viewports: u32,
    pub viewports: [Viewport; 16],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetShader {
    pub header: CommandHeader,
    pub stage: u32,
    pub shader_id: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdDraw {
    pub header: CommandHeader,
    pub vertex_count: u32,
    pub start_vertex: u32,
    pub _reserved: [u32; 2],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdDrawIndexed {
    pub header: CommandHeader,
    pub index_count: u32,
    pub start_index: u32,
    pub base_vertex: i32,
    pub _reserved: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdFence {
    pub header: CommandHeader,
    pub fence_value: u64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdPresent {
    pub header: CommandHeader,
    pub backbuffer_id: u32,
    pub sync_interval: u32,
    pub flags: u32,
    pub _reserved: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdClearRenderTarget {
    pub header: CommandHeader,
    pub rtv_id: u32,
    pub color: [f32; 4],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetVertexBuffer {
    pub header: CommandHeader,
    pub slot: u32,
    pub buffer_id: u32,
    pub stride: u32,
    pub offset: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetIndexBuffer {
    pub header: CommandHeader,
    pub buffer_id: u32,
    pub format: u32, // DXGI_FORMAT (16 = R16_UINT, 42 = R32_UINT)
    pub offset: u32,
    pub _reserved: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetConstantBuffer {
    pub header: CommandHeader,
    pub stage: u32,       // ShaderStage enum
    pub slot: u32,
    pub buffer_id: u32,
    pub _reserved: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetInputLayout {
    pub header: CommandHeader,
    pub layout_id: u32,
    pub _reserved: [u32; 3],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetPrimitiveTopology {
    pub header: CommandHeader,
    pub topology: u32, // D3D11_PRIMITIVE_TOPOLOGY
    pub _reserved: [u32; 3],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetSampler {
    pub header: CommandHeader,
    pub stage: u32,
    pub slot: u32,
    pub sampler_id: u32,
    pub _reserved: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetShaderResource {
    pub header: CommandHeader,
    pub stage: u32,
    pub slot: u32,
    pub srv_id: u32,
    pub _reserved: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetBlendState {
    pub header: CommandHeader,
    pub state_id: u32,
    pub blend_factor: [f32; 4],
    pub sample_mask: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetRasterizerState {
    pub header: CommandHeader,
    pub state_id: u32,
    pub _reserved: [u32; 3],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetDepthStencil {
    pub header: CommandHeader,
    pub state_id: u32,
    pub stencil_ref: u32,
    pub _reserved: [u32; 2],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ScissorRect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdSetScissor {
    pub header: CommandHeader,
    pub num_rects: u32,
    pub _reserved: [u32; 3],
    pub rects: [ScissorRect; 16],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdDrawInstanced {
    pub header: CommandHeader,
    pub vertex_count: u32,
    pub instance_count: u32,
    pub start_vertex: u32,
    pub start_instance: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdDrawIndexedInstanced {
    pub header: CommandHeader,
    pub index_count: u32,
    pub instance_count: u32,
    pub start_index: u32,
    pub base_vertex: i32,
    pub start_instance: u32,
    pub _reserved: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdDispatch {
    pub header: CommandHeader,
    pub thread_group_count_x: u32,
    pub thread_group_count_y: u32,
    pub thread_group_count_z: u32,
    pub _reserved: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdClearDepthStencil {
    pub header: CommandHeader,
    pub dsv_id: u32,
    pub clear_flags: u32, // D3D11_CLEAR_DEPTH = 1, D3D11_CLEAR_STENCIL = 2
    pub depth: f32,
    pub stencil: u8,
    pub _padding: [u8; 3],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdCopyResource {
    pub header: CommandHeader,
    pub dst_resource_id: u32,
    pub src_resource_id: u32,
    pub _reserved: [u32; 2],
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CmdUpdateResource {
    pub header: CommandHeader,
    pub resource_id: u32,
    pub subresource: u32,
    pub heap_offset: u32,
    pub data_size: u32,
    // Box for partial updates (optional, all zeros = full update)
    pub dst_x: u32,
    pub dst_y: u32,
    pub dst_z: u32,
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub row_pitch: u32,
    pub depth_pitch: u32,
}

// =============================================================================
// Helper Functions
// =============================================================================

/// Align a value to 16-byte boundary.
#[allow(dead_code)]
pub const fn align16(x: usize) -> usize {
    (x + 15) & !15
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_control_region_size() {
        assert_eq!(std::mem::size_of::<ControlRegion>(), 4096);
    }

    #[test]
    fn test_command_header_size() {
        assert_eq!(std::mem::size_of::<CommandHeader>(), 16);
    }
}
