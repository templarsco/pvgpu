//! D3D11 Renderer Module
//!
//! Handles D3D11 device creation and command execution.

use std::collections::HashMap;

use anyhow::Result;
use tracing::{debug, info, warn};

/// Resource ID type
pub type ResourceId = u32;

/// Holds all D3D11 resources and state
pub struct D3D11Renderer {
    // TODO: ID3D11Device
    // TODO: ID3D11DeviceContext
    // TODO: Resource map
    resources: HashMap<ResourceId, Resource>,
    next_resource_id: ResourceId,
}

/// A GPU resource (texture, buffer, shader, etc.)
pub enum Resource {
    Texture2D {
        // TODO: ID3D11Texture2D
        width: u32,
        height: u32,
        format: u32,
    },
    Buffer {
        // TODO: ID3D11Buffer
        size: u32,
        bind_flags: u32,
    },
    VertexShader {
        // TODO: ID3D11VertexShader
    },
    PixelShader {
        // TODO: ID3D11PixelShader
    },
    // ... other resource types
}

impl D3D11Renderer {
    /// Create a new D3D11 renderer with the specified adapter.
    pub fn new(_adapter_index: Option<u32>) -> Result<Self> {
        info!("Creating D3D11 device...");

        // TODO: Call D3D11CreateDevice
        // TODO: Select adapter if specified

        Ok(Self {
            resources: HashMap::new(),
            next_resource_id: 1,
        })
    }

    /// Create a resource and return its ID.
    pub fn create_resource(&mut self, resource: Resource) -> ResourceId {
        let id = self.next_resource_id;
        self.next_resource_id += 1;
        self.resources.insert(id, resource);
        debug!("Created resource {}", id);
        id
    }

    /// Destroy a resource by ID.
    pub fn destroy_resource(&mut self, id: ResourceId) -> bool {
        if self.resources.remove(&id).is_some() {
            debug!("Destroyed resource {}", id);
            true
        } else {
            warn!("Attempted to destroy non-existent resource {}", id);
            false
        }
    }

    /// Execute a draw call.
    pub fn draw(&mut self, vertex_count: u32, start_vertex: u32) {
        debug!("Draw: {} vertices from {}", vertex_count, start_vertex);
        // TODO: ID3D11DeviceContext::Draw
    }

    /// Execute an indexed draw call.
    pub fn draw_indexed(&mut self, index_count: u32, start_index: u32, base_vertex: i32) {
        debug!(
            "DrawIndexed: {} indices from {}, base {}",
            index_count, start_index, base_vertex
        );
        // TODO: ID3D11DeviceContext::DrawIndexed
    }

    /// Present the current frame.
    pub fn present(&mut self, backbuffer_id: ResourceId, sync_interval: u32) {
        debug!(
            "Present: backbuffer {}, sync {}",
            backbuffer_id, sync_interval
        );
        // TODO: Copy to presentation pipeline
    }
}
