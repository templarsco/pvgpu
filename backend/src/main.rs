//! PVGPU Host Backend Service
//!
//! This service runs on the Windows host and:
//! 1. Connects to the QEMU pvgpu device via named pipe
//! 2. Maps the shared memory region
//! 3. Processes commands from the guest via the command ring
//! 4. Executes D3D11 commands on the real GPU
//! 5. Presents frames via window or shared texture

// Allow dead code during development - this is a skeleton implementation
#![allow(dead_code)]

mod command_processor;
mod config;
mod d3d11;
mod ipc;
mod presentation;
mod protocol;
mod shmem;

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use anyhow::Result;
use tracing::{error, info, trace, warn, Level};
use tracing_subscriber::FmtSubscriber;

use crate::command_processor::CommandProcessor;
use crate::config::Config;
use crate::d3d11::D3D11Renderer;
use crate::ipc::{BackendMessage, PipeServer, QemuMessage};
use crate::presentation::{PresentationConfig, PresentationMode, PresentationPipeline};
use crate::shmem::SharedMemory;

pub use protocol::*;

/// Backend service state
struct BackendService {
    config: Config,
    pipe_server: Option<Arc<PipeServer>>,
    shared_memory: Option<SharedMemory>,
    command_processor: Option<CommandProcessor>,
    presentation: Option<PresentationPipeline>,
    shutdown: Arc<AtomicBool>,
    pipe_reader_handle: Option<thread::JoinHandle<()>>,
}

impl BackendService {
    fn new(config: Config) -> Self {
        Self {
            config,
            pipe_server: None,
            shared_memory: None,
            command_processor: None,
            presentation: None,
            shutdown: Arc::new(AtomicBool::new(false)),
            pipe_reader_handle: None,
        }
    }

    /// Initialize the pipe server and wait for QEMU connection
    fn init_pipe_server(&mut self) -> Result<()> {
        info!("Initializing named pipe server...");
        let mut server = PipeServer::new(&self.config.pipe_path)?;
        server.wait_for_connection()?;
        self.pipe_server = Some(Arc::new(server));
        Ok(())
    }

    /// Perform handshake with QEMU device
    fn perform_handshake(&mut self) -> Result<()> {
        let server = self
            .pipe_server
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("Pipe server not initialized"))?;

        info!("Waiting for handshake from QEMU...");
        let msg = server.read_message()?;

        match msg {
            QemuMessage::Handshake {
                shmem_name,
                shmem_size,
            } => {
                info!(
                    "Handshake received: shmem_name={}, size={}MB",
                    shmem_name,
                    shmem_size / (1024 * 1024)
                );

                // Open shared memory
                let shmem = SharedMemory::open(&shmem_name, shmem_size as usize)?;
                shmem.validate_control_region()?;
                self.shared_memory = Some(shmem);

                // Send handshake acknowledgement
                server.send_message(BackendMessage::HandshakeAck {
                    features: PVGPU_FEATURES_MVP,
                })?;

                info!("Handshake complete!");
                Ok(())
            }
            _ => Err(anyhow::anyhow!("Expected handshake, got {:?}", msg)),
        }
    }

    /// Initialize D3D11 renderer and presentation pipeline
    fn init_renderer(&mut self) -> Result<()> {
        info!("Initializing D3D11 renderer...");
        let renderer = D3D11Renderer::new(Some(self.config.adapter_index))?;

        // Get device and context for presentation pipeline before moving renderer
        let device = renderer.device().clone();
        let context = renderer.context().clone();

        // Create command processor with the renderer
        let processor = CommandProcessor::new(renderer);
        self.command_processor = Some(processor);

        // Initialize presentation pipeline from config
        let presentation_mode = match self.config.presentation_mode.as_str() {
            "windowed" => PresentationMode::Windowed,
            "dual" => PresentationMode::Dual,
            _ => PresentationMode::Headless,
        };
        let presentation_config = PresentationConfig {
            mode: presentation_mode,
            width: self.config.width,
            height: self.config.height,
            vsync: self.config.vsync,
            window_title: "PVGPU Output".to_string(),
            frame_event_name: Some("Global\\PVGPU_FrameEvent".to_string()),
            buffer_count: self.config.buffer_count,
            allow_tearing: !self.config.vsync,
        };

        info!("Initializing presentation pipeline...");
        let presentation = PresentationPipeline::new(device, context, presentation_config)?;

        if let Some(handle) = presentation.shared_handle() {
            info!("Shared texture handle: {:?}", handle);
        }

        self.presentation = Some(presentation);

        info!("D3D11 renderer and presentation pipeline initialized");
        Ok(())
    }

    /// Main processing loop
    fn run_loop(&mut self) -> Result<()> {
        info!("Entering main processing loop...");
        let mut device_lost_reported = false;
        let mut last_irq_fence: u64 = 0;

        loop {
            // Check for shutdown
            if self.shutdown.load(Ordering::Relaxed) {
                info!("Shutdown requested");
                break;
            }

            // Check for device lost state periodically (every iteration when idle)
            if let Some(ref processor) = self.command_processor {
                if !processor.renderer().check_device_status() && !device_lost_reported {
                    error!("D3D11 device lost!");
                    device_lost_reported = true;

                    // Report device lost to guest via control region
                    if let Some(ref shmem) = self.shared_memory {
                        shmem
                            .control_region()
                            .set_status_flag(PVGPU_STATUS_DEVICE_LOST);
                        shmem.control_region().set_error(PVGPU_ERROR_DEVICE_LOST, 0);
                    }

                    // Note: Device recovery would require recreating the D3D11 device
                    // and all resources. For now, we report the error and continue
                    // processing (commands will fail but the VM won't crash).
                    // Full recovery would be implemented in a future version.
                    warn!("Device lost - continuing in degraded mode");
                }
            }

            // Process window messages if we have a presentation pipeline
            if let Some(ref mut presentation) = self.presentation {
                if !presentation.process_messages() {
                    info!("Window closed, shutting down...");
                    break;
                }
            }

            // Process pending commands from ring buffer
            let mut processed = 0u64;
            let mut pending_present: Option<(u32, u32)> = None;

            // Scope for mutable borrows of processor and shmem
            {
                let shmem = match self.shared_memory.as_ref() {
                    Some(s) => s,
                    None => return Err(anyhow::anyhow!("Shared memory not initialized")),
                };

                let processor = match self.command_processor.as_mut() {
                    Some(p) => p,
                    None => return Err(anyhow::anyhow!("Command processor not initialized")),
                };

                let server = match self.pipe_server.as_ref() {
                    Some(s) => s,
                    None => return Err(anyhow::anyhow!("Pipe server not initialized")),
                };

                while let Some((data, _pending_count)) = shmem.read_pending_commands() {
                    if data.is_empty() {
                        break;
                    }

                    // Get the heap for data transfer commands
                    let heap = shmem.resource_heap();

                    match processor.process_command(data.as_slice(), heap) {
                        Ok(consumed) => {
                            shmem.advance_consumer(consumed as u64);
                            processed += consumed as u64;

                            // Update fence if needed — only send IRQ when a NEW
                            // fence value is completed (not on every command)
                            let fence = processor.current_fence();
                            if fence > last_irq_fence {
                                shmem.complete_fence(fence);
                                last_irq_fence = fence;
                                // Request IRQ to notify guest
                                if let Err(e) =
                                    server.send_message(BackendMessage::Irq { vector: 0 })
                                {
                                    warn!("Failed to send IRQ: {}", e);
                                }
                            }

                            // Check for pending present
                            if let Some(present_info) = processor.take_pending_present() {
                                pending_present = Some(present_info);
                            }
                        }
                        Err(e) => {
                            let err_str = e.to_string();
                            error!("Error processing command: {}", err_str);

                            // Parse error type and report via control region
                            if err_str.starts_with("SHADER_COMPILE:") {
                                // Shader compilation error - extract resource ID
                                let resource_id: u32 = err_str
                                    .strip_prefix("SHADER_COMPILE:")
                                    .and_then(|s| s.parse().ok())
                                    .unwrap_or(0);
                                shmem
                                    .control_region()
                                    .set_error(PVGPU_ERROR_SHADER_COMPILE, resource_id);
                                // Shader errors are non-fatal - continue processing
                                // The guest should handle the missing shader gracefully
                                warn!(
                                    "Shader compilation failed for resource {}, continuing...",
                                    resource_id
                                );
                            } else if err_str.contains("out of memory")
                                || err_str.contains("OutOfMemory")
                            {
                                shmem
                                    .control_region()
                                    .set_error(PVGPU_ERROR_OUT_OF_MEMORY, 0);
                                // OOM is potentially fatal - break the inner loop
                                break;
                            } else {
                                // Generic internal error
                                shmem.control_region().set_error(PVGPU_ERROR_INTERNAL, 0);
                                break;
                            }
                        }
                    }

                    // Don't process too many commands in one batch
                    if processed > 1024 * 1024 {
                        break;
                    }
                }
            }

            // Handle presentation outside the borrow scope
            if let Some((backbuffer_id, _sync_interval)) = pending_present {
                if let (Some(presentation), Some(processor)) =
                    (self.presentation.as_mut(), self.command_processor.as_ref())
                {
                    // Get the texture from the renderer
                    if let Some(texture) = processor.renderer().get_texture(backbuffer_id) {
                        if let Err(e) = presentation.present(texture) {
                            error!("Presentation failed: {}", e);
                            // Report presentation error via control region
                            if let Some(ref shmem) = self.shared_memory {
                                shmem
                                    .control_region()
                                    .set_error(PVGPU_ERROR_DEVICE_LOST, backbuffer_id);
                            }
                        }
                    } else {
                        warn!("Present: backbuffer {} not found", backbuffer_id);
                        // Report resource not found error
                        if let Some(ref shmem) = self.shared_memory {
                            shmem
                                .control_region()
                                .set_error(PVGPU_ERROR_RESOURCE_NOT_FOUND, backbuffer_id);
                        }
                    }
                }
            }

            // Handle pending resize outside the borrow scope
            if let Some(processor) = self.command_processor.as_mut() {
                if let Some((width, height)) = processor.take_pending_resize() {
                    // Set resizing status
                    if let Some(ref shmem) = self.shared_memory {
                        shmem
                            .control_region()
                            .set_status_flag(PVGPU_STATUS_RESIZING);
                    }

                    if let Some(presentation) = self.presentation.as_mut() {
                        if let Err(e) = presentation.resize(width, height) {
                            error!("Resize failed: {}", e);
                            // Report resize error
                            if let Some(ref shmem) = self.shared_memory {
                                shmem.control_region().set_error(
                                    PVGPU_ERROR_INTERNAL,
                                    (width & 0xFFFF) | ((height & 0xFFFF) << 16),
                                );
                            }
                        } else {
                            info!("Resized presentation to {}x{}", width, height);
                        }
                    }

                    // Clear resizing status
                    if let Some(ref shmem) = self.shared_memory {
                        shmem
                            .control_region()
                            .clear_status_flag(PVGPU_STATUS_RESIZING);
                    }
                }
            }

            // If we processed commands, continue immediately
            if processed > 0 {
                continue;
            }

            // No commands available, wait for doorbell event or timeout.
            // The doorbell event is signaled by the pipe reader thread when
            // QEMU notifies us of new commands. We use a short timeout to
            // handle window messages and device status checks.
            if let Some(server) = &self.pipe_server {
                server.wait_for_doorbell(5);
            } else {
                std::thread::sleep(Duration::from_micros(100));
            }
        }

        Ok(())
    }

    /// Request shutdown
    fn request_shutdown(&self) {
        self.shutdown.store(true, Ordering::Relaxed);
        if let Some(server) = &self.pipe_server {
            server.signal_shutdown();
        }
    }

    /// Start background pipe reader thread
    ///
    /// Reads messages from the QEMU pipe in a loop. Doorbell messages
    /// are automatically handled by PipeServer::read_message() which
    /// signals the doorbell event. Other messages are logged.
    fn start_pipe_reader(&mut self) {
        let server = self
            .pipe_server
            .as_ref()
            .expect("Pipe server not initialized")
            .clone();
        let shutdown = self.shutdown.clone();

        let handle = thread::Builder::new()
            .name("pvgpu-pipe-reader".to_string())
            .spawn(move || {
                info!("Pipe reader thread started");
                loop {
                    if shutdown.load(Ordering::Relaxed) {
                        break;
                    }
                    if server.is_shutdown_signaled() {
                        break;
                    }
                    match server.read_message() {
                        Ok(msg) => match msg {
                            QemuMessage::Doorbell => {
                                // Doorbell already signals the event inside read_message()
                                trace!("Doorbell received from QEMU");
                            }
                            QemuMessage::Shutdown => {
                                info!("Shutdown message received from QEMU");
                                shutdown.store(true, Ordering::Relaxed);
                                server.signal_shutdown();
                                break;
                            }
                            QemuMessage::Handshake { .. } => {
                                warn!("Unexpected handshake message during operation");
                            }
                        },
                        Err(e) => {
                            if !shutdown.load(Ordering::Relaxed) {
                                error!("Pipe read error: {}", e);
                                // On read error, signal shutdown
                                shutdown.store(true, Ordering::Relaxed);
                                server.signal_shutdown();
                            }
                            break;
                        }
                    }
                }
                info!("Pipe reader thread exiting");
            })
            .expect("Failed to spawn pipe reader thread");

        self.pipe_reader_handle = Some(handle);
    }
}

fn main() -> Result<()> {
    // Initialize logging
    FmtSubscriber::builder()
        .with_max_level(Level::DEBUG)
        .with_target(true)
        .init();

    info!("PVGPU Backend Service starting...");
    info!(
        "Protocol version: {}.{}",
        PVGPU_VERSION_MAJOR, PVGPU_VERSION_MINOR
    );

    // Load or create default config
    let config = Config::default();
    info!("Configuration loaded: {:?}", config);

    // Create service
    let mut service = BackendService::new(config);

    // Setup Ctrl+C handler
    let shutdown = service.shutdown.clone();
    ctrlc::set_handler(move || {
        info!("Ctrl+C received, shutting down...");
        shutdown.store(true, Ordering::Relaxed);
    })
    .expect("Error setting Ctrl+C handler");

    // Initialize pipe server and wait for connection
    service.init_pipe_server()?;

    // Perform handshake
    service.perform_handshake()?;

    // Initialize D3D11
    service.init_renderer()?;

    // Start pipe reader thread (reads doorbell/shutdown messages from QEMU)
    service.start_pipe_reader();

    // Set ready status in shared memory
    if let Some(ref shmem) = service.shared_memory {
        shmem.control_region().set_status(PVGPU_STATUS_READY);
        info!("Device status set to READY");
    }

    // Run main loop
    info!("Backend service ready. Processing commands...");
    let result = service.run_loop();

    // Set shutdown status before exiting
    if let Some(ref shmem) = service.shared_memory {
        shmem.control_region().set_status(PVGPU_STATUS_SHUTDOWN);
        info!("Device status set to SHUTDOWN");
    }

    info!("Backend service shutting down");

    // Wait for pipe reader thread to finish
    if let Some(handle) = service.pipe_reader_handle.take() {
        let _ = handle.join();
    }

    result
}
