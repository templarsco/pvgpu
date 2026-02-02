//! Shared Memory Module
//!
//! Maps the shared memory region created by QEMU for command ring
//! and resource heap access.

use std::slice;
use std::sync::atomic::{AtomicBool, Ordering};

use anyhow::{anyhow, Result};
use tracing::{debug, info};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{CloseHandle, HANDLE};
use windows::Win32::System::Memory::{
    MapViewOfFile, OpenFileMappingW, UnmapViewOfFile, FILE_MAP_ALL_ACCESS,
    MEMORY_MAPPED_VIEW_ADDRESS,
};

use crate::protocol::{ControlRegion, PVGPU_MAGIC, PVGPU_VERSION_MAJOR};

/// Shared memory region mapped from QEMU
pub struct SharedMemory {
    /// Handle to the file mapping object
    mapping_handle: HANDLE,
    /// Base address of the mapped view
    base_addr: *mut u8,
    /// Total size of the mapped region
    size: usize,
    /// Whether the region is valid and initialized
    initialized: AtomicBool,
}

// SAFETY: SharedMemory handles are valid across threads
unsafe impl Send for SharedMemory {}
unsafe impl Sync for SharedMemory {}

impl SharedMemory {
    /// Open and map a shared memory region by name
    pub fn open(name: &str, expected_size: usize) -> Result<Self> {
        info!(
            "Opening shared memory: {} (size: {} bytes)",
            name, expected_size
        );

        // Convert name to wide string
        let wide_name: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();

        // Open the file mapping object
        let handle =
            unsafe { OpenFileMappingW(FILE_MAP_ALL_ACCESS.0, false, PCWSTR(wide_name.as_ptr()))? };

        if handle.is_invalid() {
            return Err(anyhow!("Failed to open file mapping: {}", name));
        }

        // Map the entire region
        let view = unsafe { MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, expected_size) };

        if view.Value.is_null() {
            unsafe {
                let _ = CloseHandle(handle);
            }
            return Err(anyhow!("Failed to map view of file"));
        }

        info!("Shared memory mapped at {:p}", view.Value);

        Ok(Self {
            mapping_handle: handle,
            base_addr: view.Value as *mut u8,
            size: expected_size,
            initialized: AtomicBool::new(false),
        })
    }

    /// Validate and initialize the control region
    pub fn validate_control_region(&self) -> Result<()> {
        let control = self.control_region();

        // Check magic number
        if control.magic != PVGPU_MAGIC {
            return Err(anyhow!(
                "Invalid magic number: expected 0x{:08X}, got 0x{:08X}",
                PVGPU_MAGIC,
                control.magic
            ));
        }

        // Check version compatibility
        let major = control.version >> 16;
        if major != PVGPU_VERSION_MAJOR {
            return Err(anyhow!(
                "Incompatible protocol version: expected major {}, got {}",
                PVGPU_VERSION_MAJOR,
                major
            ));
        }

        info!(
            "Control region validated: version {}.{}, features 0x{:016X}",
            major,
            control.version & 0xFFFF,
            control.features
        );

        self.initialized.store(true, Ordering::Release);
        Ok(())
    }

    /// Get a reference to the control region
    pub fn control_region(&self) -> &ControlRegion {
        // SAFETY: Control region is at offset 0 and properly aligned
        unsafe { &*(self.base_addr as *const ControlRegion) }
    }

    /// Get a mutable reference to the control region
    ///
    /// # Safety
    /// Caller must ensure exclusive access
    pub unsafe fn control_region_mut(&self) -> &mut ControlRegion {
        &mut *(self.base_addr as *mut ControlRegion)
    }

    /// Get a slice of the command ring
    pub fn command_ring(&self) -> &[u8] {
        let control = self.control_region();
        let offset = control.ring_offset as usize;
        let size = control.ring_size as usize;

        // SAFETY: Ring buffer is within the mapped region
        unsafe { slice::from_raw_parts(self.base_addr.add(offset), size) }
    }

    /// Get a mutable slice of the command ring
    ///
    /// # Safety
    /// Caller must ensure proper synchronization
    pub unsafe fn command_ring_mut(&self) -> &mut [u8] {
        let control = self.control_region();
        let offset = control.ring_offset as usize;
        let size = control.ring_size as usize;

        slice::from_raw_parts_mut(self.base_addr.add(offset), size)
    }

    /// Get a slice of the resource heap
    pub fn resource_heap(&self) -> &[u8] {
        let control = self.control_region();
        let offset = control.heap_offset as usize;
        let size = control.heap_size as usize;

        // SAFETY: Heap is within the mapped region
        unsafe { slice::from_raw_parts(self.base_addr.add(offset), size) }
    }

    /// Get a mutable slice of the resource heap
    ///
    /// # Safety
    /// Caller must ensure proper synchronization
    pub unsafe fn resource_heap_mut(&self) -> &mut [u8] {
        let control = self.control_region();
        let offset = control.heap_offset as usize;
        let size = control.heap_size as usize;

        slice::from_raw_parts_mut(self.base_addr.add(offset), size)
    }

    /// Read commands from the ring buffer starting at the consumer pointer
    /// Returns the data slice and the number of pending bytes
    pub fn read_pending_commands(&self) -> Option<(&[u8], u64)> {
        let control = self.control_region();
        let pending = control.pending_bytes();

        if pending == 0 {
            return None;
        }

        let ring = self.command_ring();
        let ring_size = ring.len() as u64;
        let consumer = control.consumer_ptr();

        // Calculate offset within ring (wrap around)
        let offset = (consumer % ring_size) as usize;
        let available = std::cmp::min(pending as usize, ring.len() - offset);

        Some((&ring[offset..offset + available], pending))
    }

    /// Advance the consumer pointer after processing commands
    pub fn advance_consumer(&self, bytes: u64) {
        let control = self.control_region();
        let new_consumer = control.consumer_ptr() + bytes;
        control.set_consumer_ptr(new_consumer);
        debug!("Consumer pointer advanced to {}", new_consumer);
    }

    /// Update the host fence completed value
    pub fn complete_fence(&self, fence_value: u64) {
        let control = self.control_region();
        control.set_host_fence_completed(fence_value);
        debug!("Host fence completed: {}", fence_value);
    }

    /// Get the total size of the mapped region
    pub fn size(&self) -> usize {
        self.size
    }

    /// Check if the region is initialized
    pub fn is_initialized(&self) -> bool {
        self.initialized.load(Ordering::Acquire)
    }

    /// Get a raw pointer to an offset within the shared memory
    ///
    /// # Safety
    /// Caller must ensure offset is within bounds
    pub unsafe fn ptr_at_offset(&self, offset: usize) -> *mut u8 {
        debug_assert!(offset < self.size);
        self.base_addr.add(offset)
    }
}

impl Drop for SharedMemory {
    fn drop(&mut self) {
        if !self.base_addr.is_null() {
            info!("Unmapping shared memory");
            unsafe {
                let view = MEMORY_MAPPED_VIEW_ADDRESS {
                    Value: self.base_addr as *mut _,
                };
                let _ = UnmapViewOfFile(view);
            }
        }

        if !self.mapping_handle.is_invalid() {
            unsafe {
                let _ = CloseHandle(self.mapping_handle);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_shared_memory_size() {
        // Just verify the struct is properly sized
        assert!(std::mem::size_of::<SharedMemory>() > 0);
    }
}
