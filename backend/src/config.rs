//! Configuration Module
//!
//! Handles configuration file parsing and command-line arguments.

use std::path::Path;

use anyhow::Result;
use serde::{Deserialize, Serialize};

/// Backend configuration
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Config {
    /// Named pipe path for QEMU connection
    #[serde(default = "default_pipe_path")]
    pub pipe_path: String,

    /// Shared memory file path (Windows file mapping)
    #[serde(default)]
    pub shmem_path: Option<String>,

    /// GPU adapter index (0 = default)
    #[serde(default)]
    pub adapter_index: u32,

    /// Presentation mode: "headless", "windowed", "dual"
    #[serde(default = "default_presentation_mode")]
    pub presentation_mode: String,

    /// Initial display width
    #[serde(default = "default_width")]
    pub width: u32,

    /// Initial display height
    #[serde(default = "default_height")]
    pub height: u32,

    /// VSync enabled
    #[serde(default = "default_vsync")]
    pub vsync: bool,

    /// Number of frame buffers (2 or 3)
    #[serde(default = "default_buffer_count")]
    pub buffer_count: u32,
}

fn default_pipe_path() -> String {
    r"\\.\pipe\pvgpu".to_string()
}

fn default_presentation_mode() -> String {
    "headless".to_string()
}

fn default_width() -> u32 {
    1920
}

fn default_height() -> u32 {
    1080
}

fn default_vsync() -> bool {
    true
}

fn default_buffer_count() -> u32 {
    2
}

impl Default for Config {
    fn default() -> Self {
        Self {
            pipe_path: default_pipe_path(),
            shmem_path: None,
            adapter_index: 0,
            presentation_mode: default_presentation_mode(),
            width: default_width(),
            height: default_height(),
            vsync: default_vsync(),
            buffer_count: default_buffer_count(),
        }
    }
}

impl Config {
    /// Load configuration from a TOML file.
    pub fn load<P: AsRef<Path>>(path: P) -> Result<Self> {
        let content = std::fs::read_to_string(path)?;
        let config: Config = toml::from_str(&content)?;
        Ok(config)
    }

    /// Save configuration to a TOML file.
    pub fn save<P: AsRef<Path>>(&self, path: P) -> Result<()> {
        let content = toml::to_string_pretty(self)?;
        std::fs::write(path, content)?;
        Ok(())
    }
}
