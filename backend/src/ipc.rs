//! IPC Module - Named Pipe Server
//!
//! Handles communication with the QEMU pvgpu device via named pipes.
//! The QEMU device connects to this server to:
//! 1. Exchange handshake messages
//! 2. Send doorbell notifications when new commands are available
//! 3. Receive IRQ requests from host

use std::ffi::c_void;

use anyhow::{anyhow, Result};
use tokio::sync::mpsc;
use tracing::{debug, info};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{
    CloseHandle, GetLastError, HANDLE, INVALID_HANDLE_VALUE, WAIT_OBJECT_0,
};
use windows::Win32::Storage::FileSystem::{
    FlushFileBuffers, ReadFile, WriteFile, FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_ACCESS_DUPLEX,
};
use windows::Win32::System::Pipes::{
    ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, PIPE_READMODE_MESSAGE,
    PIPE_TYPE_MESSAGE, PIPE_UNLIMITED_INSTANCES, PIPE_WAIT,
};
use windows::Win32::System::Threading::{CreateEventW, SetEvent, WaitForSingleObject};

/// Messages from QEMU device to backend
#[derive(Debug, Clone)]
pub enum QemuMessage {
    /// QEMU connected, provides shared memory handle name
    Handshake { shmem_name: String, shmem_size: u64 },
    /// Doorbell notification - new commands in ring
    Doorbell,
    /// QEMU is shutting down
    Shutdown,
}

/// Messages from backend to QEMU device
#[derive(Debug, Clone)]
pub enum BackendMessage {
    /// Handshake accepted, ready to process
    HandshakeAck { features: u64 },
    /// Request QEMU to send IRQ to guest
    Irq { vector: u32 },
}

/// Wire protocol message types
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum MessageType {
    Handshake = 1,
    HandshakeAck = 2,
    Doorbell = 3,
    Irq = 4,
    Shutdown = 5,
}

/// Wire protocol header
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
struct MessageHeader {
    msg_type: u32,
    payload_size: u32,
}

const HEADER_SIZE: usize = std::mem::size_of::<MessageHeader>();

/// Named pipe server for QEMU communication
pub struct PipeServer {
    pipe_path: String,
    pipe_handle: HANDLE,
    shutdown_event: HANDLE,
}

impl PipeServer {
    /// Create a new named pipe server (but don't start listening yet)
    pub fn new(pipe_path: &str) -> Result<Self> {
        info!("Creating named pipe server at: {}", pipe_path);

        // Create shutdown event
        let shutdown_event = unsafe { CreateEventW(None, true, false, None)? };

        Ok(Self {
            pipe_path: pipe_path.to_string(),
            pipe_handle: INVALID_HANDLE_VALUE,
            shutdown_event,
        })
    }

    /// Create the named pipe and wait for a client connection
    pub fn wait_for_connection(&mut self) -> Result<()> {
        // Convert path to wide string
        let wide_path: Vec<u16> = self
            .pipe_path
            .encode_utf16()
            .chain(std::iter::once(0))
            .collect();

        // Create the named pipe
        let pipe = unsafe {
            CreateNamedPipeW(
                PCWSTR(wide_path.as_ptr()),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                4096, // Out buffer size
                4096, // In buffer size
                0,    // Default timeout
                None, // Default security
            )?
        };

        if pipe == INVALID_HANDLE_VALUE {
            return Err(anyhow!("Failed to create named pipe"));
        }

        self.pipe_handle = pipe;
        info!("Named pipe created, waiting for QEMU connection...");

        // Wait for client connection (blocking)
        let connected = unsafe { ConnectNamedPipe(pipe, None) };

        if connected.is_err() {
            // Check if already connected (ERROR_PIPE_CONNECTED)
            let error = unsafe { GetLastError() };
            if error.0 != 535 {
                // ERROR_PIPE_CONNECTED
                return Err(anyhow!("ConnectNamedPipe failed: {:?}", error));
            }
        }

        info!("QEMU device connected!");
        Ok(())
    }

    /// Read a message from QEMU
    pub fn read_message(&self) -> Result<QemuMessage> {
        // Read header
        let mut header = MessageHeader {
            msg_type: 0,
            payload_size: 0,
        };
        let mut bytes_read: u32 = 0;

        unsafe {
            ReadFile(
                self.pipe_handle,
                Some(&mut header as *mut _ as *mut c_void as *mut u8 as *mut [u8]),
                Some(&mut bytes_read),
                None,
            )?;
        }

        if bytes_read as usize != HEADER_SIZE {
            return Err(anyhow!("Incomplete header read: {} bytes", bytes_read));
        }

        // Read payload if present
        let mut payload = vec![0u8; header.payload_size as usize];
        if header.payload_size > 0 {
            let mut payload_read: u32 = 0;
            unsafe {
                ReadFile(
                    self.pipe_handle,
                    Some(payload.as_mut_slice()),
                    Some(&mut payload_read),
                    None,
                )?;
            }
        }

        // Parse message
        match header.msg_type {
            1 => {
                // Handshake
                // Payload format: shmem_size (u64) + shmem_name (null-terminated string)
                if payload.len() < 8 {
                    return Err(anyhow!("Handshake payload too small"));
                }
                let shmem_size = u64::from_le_bytes(payload[0..8].try_into()?);
                let shmem_name = String::from_utf8_lossy(&payload[8..])
                    .trim_end_matches('\0')
                    .to_string();
                debug!(
                    "Received handshake: shmem_name={}, size={}",
                    shmem_name, shmem_size
                );
                Ok(QemuMessage::Handshake {
                    shmem_name,
                    shmem_size,
                })
            }
            3 => {
                // Doorbell
                debug!("Received doorbell");
                Ok(QemuMessage::Doorbell)
            }
            5 => {
                // Shutdown
                info!("Received shutdown from QEMU");
                Ok(QemuMessage::Shutdown)
            }
            _ => Err(anyhow!("Unknown message type: {}", header.msg_type)),
        }
    }

    /// Send a message to QEMU
    pub fn send_message(&self, msg: BackendMessage) -> Result<()> {
        let (msg_type, payload) = match msg {
            BackendMessage::HandshakeAck { features } => (2u32, features.to_le_bytes().to_vec()),
            BackendMessage::Irq { vector } => (4u32, vector.to_le_bytes().to_vec()),
        };

        let header = MessageHeader {
            msg_type,
            payload_size: payload.len() as u32,
        };

        // Write header
        let mut bytes_written: u32 = 0;
        unsafe {
            WriteFile(
                self.pipe_handle,
                Some(std::slice::from_raw_parts(
                    &header as *const _ as *const u8,
                    HEADER_SIZE,
                )),
                Some(&mut bytes_written),
                None,
            )?;
        }

        // Write payload
        if !payload.is_empty() {
            unsafe {
                WriteFile(
                    self.pipe_handle,
                    Some(&payload),
                    Some(&mut bytes_written),
                    None,
                )?;
            }
        }

        unsafe {
            FlushFileBuffers(self.pipe_handle)?;
        }

        Ok(())
    }

    /// Signal shutdown
    pub fn signal_shutdown(&self) {
        unsafe {
            let _ = SetEvent(self.shutdown_event);
        }
    }

    /// Check if shutdown was signaled
    pub fn is_shutdown_signaled(&self) -> bool {
        unsafe { WaitForSingleObject(self.shutdown_event, 0) == WAIT_OBJECT_0 }
    }

    /// Disconnect current client
    pub fn disconnect(&mut self) {
        if self.pipe_handle != INVALID_HANDLE_VALUE {
            unsafe {
                let _ = DisconnectNamedPipe(self.pipe_handle);
                let _ = CloseHandle(self.pipe_handle);
            }
            self.pipe_handle = INVALID_HANDLE_VALUE;
        }
    }
}

impl Drop for PipeServer {
    fn drop(&mut self) {
        self.disconnect();
        if !self.shutdown_event.is_invalid() {
            unsafe {
                let _ = CloseHandle(self.shutdown_event);
            }
        }
    }
}

/// Async wrapper for pipe server using Tokio
pub struct AsyncPipeServer {
    inner: PipeServer,
    msg_tx: mpsc::Sender<QemuMessage>,
    msg_rx: mpsc::Receiver<QemuMessage>,
}

impl AsyncPipeServer {
    pub fn new(pipe_path: &str) -> Result<Self> {
        let inner = PipeServer::new(pipe_path)?;
        let (msg_tx, msg_rx) = mpsc::channel(64);
        Ok(Self {
            inner,
            msg_tx,
            msg_rx,
        })
    }

    /// Run the pipe server in a blocking task
    pub async fn run(&mut self) -> Result<()> {
        // Wait for connection in blocking task
        let pipe_path = self.inner.pipe_path.clone();
        let result = tokio::task::spawn_blocking(move || {
            let mut server = PipeServer::new(&pipe_path)?;
            server.wait_for_connection()?;
            Ok::<_, anyhow::Error>(server)
        })
        .await??;

        self.inner = result;
        Ok(())
    }

    /// Get sender for QEMU messages
    pub fn message_sender(&self) -> mpsc::Sender<QemuMessage> {
        self.msg_tx.clone()
    }
}
