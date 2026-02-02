//! Presentation Pipeline Module
//!
//! Handles frame output via window or shared texture for streaming.

use anyhow::Result;
use tracing::{debug, info};

/// Presentation mode
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PresentationMode {
    /// Render to texture only (for streaming tools)
    Headless,
    /// Create a window and present frames
    Windowed,
    /// Both headless and windowed
    Dual,
}

/// Manages frame presentation
pub struct PresentationPipeline {
    mode: PresentationMode,
    width: u32,
    height: u32,
    vsync: bool,
    // TODO: Window handle (if windowed)
    // TODO: Swapchain
    // TODO: Shared texture
    // TODO: Frame event
}

impl PresentationPipeline {
    pub fn new(mode: PresentationMode, width: u32, height: u32, vsync: bool) -> Result<Self> {
        info!(
            "Creating presentation pipeline: {:?} {}x{} vsync={}",
            mode, width, height, vsync
        );

        // TODO: Create window if windowed mode
        // TODO: Create swapchain
        // TODO: Create shared texture for streaming
        // TODO: Create named event for frame signaling

        Ok(Self {
            mode,
            width,
            height,
            vsync,
        })
    }

    /// Present a frame from the given texture.
    pub fn present(&mut self, _texture_id: u32) -> Result<()> {
        debug!("Presenting frame");

        // TODO: Copy texture to swapchain backbuffer
        // TODO: Present swapchain
        // TODO: Signal frame event for streaming tools

        Ok(())
    }

    /// Resize the presentation surface.
    pub fn resize(&mut self, width: u32, height: u32) -> Result<()> {
        info!("Resizing presentation to {}x{}", width, height);
        self.width = width;
        self.height = height;

        // TODO: Recreate swapchain
        // TODO: Recreate shared texture

        Ok(())
    }

    /// Get current dimensions.
    pub fn dimensions(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    /// Get the presentation mode.
    #[allow(dead_code)]
    pub fn mode(&self) -> PresentationMode {
        self.mode
    }

    /// Check if vsync is enabled.
    #[allow(dead_code)]
    pub fn vsync(&self) -> bool {
        self.vsync
    }
}
