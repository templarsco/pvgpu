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
use std::time::Duration;

use anyhow::Result;
use tracing::{error, info, warn, Level};
use tracing_subscriber::FmtSubscriber;

use crate::command_processor::CommandProcessor;
use crate::config::Config;
use crate::d3d11::D3D11Renderer;
use crate::ipc::{BackendMessage, PipeServer, QemuMessage};
use crate::shmem::SharedMemory;

pub use protocol::*;

/// Backend service state
struct BackendService {
    config: Config,
    pipe_server: Option<PipeServer>,
    shared_memory: Option<SharedMemory>,
    renderer: Option<D3D11Renderer>,
    command_processor: Option<CommandProcessor>,
    shutdown: Arc<AtomicBool>,
}

impl BackendService {
    fn new(config: Config) -> Self {
        Self {
            config,
            pipe_server: None,
            shared_memory: None,
            renderer: None,
            command_processor: None,
            shutdown: Arc::new(AtomicBool::new(false)),
        }
    }

    /// Initialize the pipe server and wait for QEMU connection
    fn init_pipe_server(&mut self) -> Result<()> {
        info!("Initializing named pipe server...");
        let mut server = PipeServer::new(&self.config.pipe_path)?;
        server.wait_for_connection()?;
        self.pipe_server = Some(server);
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

    /// Initialize D3D11 renderer
    fn init_renderer(&mut self) -> Result<()> {
        info!("Initializing D3D11 renderer...");
        let renderer = D3D11Renderer::new(Some(self.config.adapter_index))?;
        let processor = CommandProcessor::new(renderer);

        // Store a new renderer for the service (processor takes ownership of one)
        self.renderer = Some(D3D11Renderer::new(Some(self.config.adapter_index))?);
        self.command_processor = Some(processor);

        info!("D3D11 renderer initialized");
        Ok(())
    }

    /// Main processing loop
    fn run_loop(&mut self) -> Result<()> {
        let shmem = self
            .shared_memory
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("Shared memory not initialized"))?;

        let processor = self
            .command_processor
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("Command processor not initialized"))?;

        let server = self
            .pipe_server
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("Pipe server not initialized"))?;

        info!("Entering main processing loop...");

        loop {
            // Check for shutdown
            if self.shutdown.load(Ordering::Relaxed) {
                info!("Shutdown requested");
                break;
            }

            // Process pending commands from ring buffer
            let mut processed = 0u64;
            while let Some((data, _pending)) = shmem.read_pending_commands() {
                if data.is_empty() {
                    break;
                }

                match processor.process_command(data) {
                    Ok(consumed) => {
                        shmem.advance_consumer(consumed as u64);
                        processed += consumed as u64;

                        // Update fence if needed
                        let fence = processor.current_fence();
                        if fence > 0 {
                            shmem.complete_fence(fence);
                            // Request IRQ to notify guest
                            if let Err(e) = server.send_message(BackendMessage::Irq { vector: 0 }) {
                                warn!("Failed to send IRQ: {}", e);
                            }
                        }
                    }
                    Err(e) => {
                        error!("Error processing command: {}", e);
                        break;
                    }
                }

                // Don't process too many commands in one batch
                if processed > 1024 * 1024 {
                    break;
                }
            }

            // If we processed commands, continue immediately
            if processed > 0 {
                continue;
            }

            // No commands available, wait for doorbell or timeout
            // TODO: Implement proper event-based waiting
            std::thread::sleep(Duration::from_micros(100));

            // Check for messages from QEMU (non-blocking would be better)
            // For now, we'll poll periodically
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

    // Run main loop
    info!("Backend service ready. Processing commands...");
    service.run_loop()?;

    info!("Backend service shutting down");
    Ok(())
}
