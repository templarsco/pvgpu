//! Presentation Pipeline Module
//!
//! Handles frame output via window or shared texture for streaming.
//! Supports:
//! - Windowed mode: Creates a Win32 window with DXGI swapchain
//! - Headless mode: Shared texture only (for streaming tools like Parsec/Moonlight)
//! - Dual mode: Both window and shared texture

use anyhow::{anyhow, Result};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use tracing::{debug, error, info};
use windows::core::{w, PCWSTR};
use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows::Win32::Graphics::Direct3D11::{
    ID3D11Device, ID3D11DeviceContext, ID3D11RenderTargetView, ID3D11Texture2D,
    D3D11_BIND_RENDER_TARGET, D3D11_BIND_SHADER_RESOURCE, D3D11_BOX, D3D11_RESOURCE_MISC_SHARED,
    D3D11_RESOURCE_MISC_SHARED_NTHANDLE, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT,
};
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SAMPLE_DESC};
use windows::Win32::Graphics::Dxgi::{
    IDXGIFactory2, IDXGISwapChain1, DXGI_PRESENT, DXGI_SWAP_CHAIN_DESC1, DXGI_SWAP_EFFECT_DISCARD,
    DXGI_USAGE_RENDER_TARGET_OUTPUT,
};
use windows::Win32::System::Threading::{CreateEventW, SetEvent};
use windows::Win32::UI::WindowsAndMessaging::{
    AdjustWindowRect, CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW,
    PeekMessageW, PostQuitMessage, RegisterClassExW, ShowWindow, TranslateMessage, CS_HREDRAW,
    CS_VREDRAW, CW_USEDEFAULT, MSG, PM_REMOVE, SW_SHOW, WM_CLOSE, WM_DESTROY, WM_ERASEBKGND,
    WM_PAINT, WM_SIZE, WNDCLASSEXW, WS_EX_APPWINDOW, WS_OVERLAPPEDWINDOW,
};

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

/// Configuration for presentation pipeline
#[derive(Debug, Clone)]
pub struct PresentationConfig {
    pub mode: PresentationMode,
    pub width: u32,
    pub height: u32,
    pub vsync: bool,
    pub window_title: String,
    /// Name for the shared texture event (e.g., "Global\\PVGPU_FrameEvent")
    pub frame_event_name: Option<String>,
}

impl Default for PresentationConfig {
    fn default() -> Self {
        Self {
            mode: PresentationMode::Windowed,
            width: 1920,
            height: 1080,
            vsync: true,
            window_title: "PVGPU Output".to_string(),
            frame_event_name: Some("Global\\PVGPU_FrameEvent".to_string()),
        }
    }
}

/// Manages frame presentation
pub struct PresentationPipeline {
    config: PresentationConfig,
    device: ID3D11Device,
    context: ID3D11DeviceContext,

    // Window resources
    hwnd: Option<HWND>,
    swapchain: Option<IDXGISwapChain1>,
    backbuffer_rtv: Option<ID3D11RenderTargetView>,

    // Shared texture for streaming
    shared_texture: Option<ID3D11Texture2D>,
    shared_handle: Option<windows::Win32::Foundation::HANDLE>,

    // Frame signaling
    frame_event: Option<windows::Win32::Foundation::HANDLE>,

    // Window class registered flag
    window_class_registered: bool,

    // Shutdown flag
    shutdown: Arc<AtomicBool>,
}

impl PresentationPipeline {
    /// Create a new presentation pipeline
    pub fn new(
        device: ID3D11Device,
        context: ID3D11DeviceContext,
        config: PresentationConfig,
    ) -> Result<Self> {
        info!(
            "Creating presentation pipeline: {:?} {}x{} vsync={}",
            config.mode, config.width, config.height, config.vsync
        );

        let mut pipeline = Self {
            config: config.clone(),
            device,
            context,
            hwnd: None,
            swapchain: None,
            backbuffer_rtv: None,
            shared_texture: None,
            shared_handle: None,
            frame_event: None,
            window_class_registered: false,
            shutdown: Arc::new(AtomicBool::new(false)),
        };

        // Create window if needed
        if config.mode == PresentationMode::Windowed || config.mode == PresentationMode::Dual {
            pipeline.create_window()?;
            pipeline.create_swapchain()?;
        }

        // Create shared texture if needed
        if config.mode == PresentationMode::Headless || config.mode == PresentationMode::Dual {
            pipeline.create_shared_texture()?;
        }

        // Create frame event for signaling
        if let Some(ref event_name) = config.frame_event_name {
            pipeline.create_frame_event(event_name)?;
        }

        Ok(pipeline)
    }

    /// Create the Win32 window
    fn create_window(&mut self) -> Result<()> {
        info!("Creating presentation window");

        let class_name = w!("PVGPUWindowClass");

        // Register window class if not already done
        if !self.window_class_registered {
            let wc = WNDCLASSEXW {
                cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
                style: CS_HREDRAW | CS_VREDRAW,
                lpfnWndProc: Some(window_proc),
                cbClsExtra: 0,
                cbWndExtra: 0,
                hInstance: unsafe {
                    windows::Win32::System::LibraryLoader::GetModuleHandleW(None)?
                }
                .into(),
                hIcon: Default::default(),
                hCursor: Default::default(),
                hbrBackground: Default::default(),
                lpszMenuName: PCWSTR::null(),
                lpszClassName: class_name,
                hIconSm: Default::default(),
            };

            let atom = unsafe { RegisterClassExW(&wc) };
            if atom == 0 {
                return Err(anyhow!("Failed to register window class"));
            }
            self.window_class_registered = true;
        }

        // Calculate window size to get desired client area
        let mut rect = RECT {
            left: 0,
            top: 0,
            right: self.config.width as i32,
            bottom: self.config.height as i32,
        };

        unsafe {
            let _ = AdjustWindowRect(&mut rect, WS_OVERLAPPEDWINDOW, false);
        }

        let window_width = rect.right - rect.left;
        let window_height = rect.bottom - rect.top;

        // Convert title to wide string
        let title: Vec<u16> = self
            .config
            .window_title
            .encode_utf16()
            .chain(std::iter::once(0))
            .collect();

        // Create window
        let hwnd = unsafe {
            CreateWindowExW(
                WS_EX_APPWINDOW,
                class_name,
                PCWSTR(title.as_ptr()),
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                window_width,
                window_height,
                None,
                None,
                windows::Win32::System::LibraryLoader::GetModuleHandleW(None)?,
                None,
            )?
        };

        if hwnd.0.is_null() {
            return Err(anyhow!("Failed to create window"));
        }

        unsafe {
            let _ = ShowWindow(hwnd, SW_SHOW);
        }

        self.hwnd = Some(hwnd);
        info!("Window created: {:?}", hwnd);

        Ok(())
    }

    /// Create DXGI swapchain
    fn create_swapchain(&mut self) -> Result<()> {
        let hwnd = self.hwnd.ok_or_else(|| anyhow!("No window created"))?;

        info!("Creating swapchain for window");

        // Get DXGI device and factory
        let dxgi_device: windows::Win32::Graphics::Dxgi::IDXGIDevice =
            self.device.cast()?;
        let dxgi_adapter = unsafe { dxgi_device.GetAdapter()? };
        let dxgi_factory: IDXGIFactory2 = unsafe { dxgi_adapter.GetParent()? };

        // Swapchain description
        let desc = DXGI_SWAP_CHAIN_DESC1 {
            Width: self.config.width,
            Height: self.config.height,
            Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            Stereo: false.into(),
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            BufferUsage: DXGI_USAGE_RENDER_TARGET_OUTPUT,
            BufferCount: 2,
            Scaling: windows::Win32::Graphics::Dxgi::DXGI_SCALING_STRETCH,
            SwapEffect: DXGI_SWAP_EFFECT_DISCARD,
            AlphaMode: windows::Win32::Graphics::Dxgi::DXGI_ALPHA_MODE_IGNORE,
            Flags: 0,
        };

        let swapchain =
            unsafe { dxgi_factory.CreateSwapChainForHwnd(&self.device, hwnd, &desc, None, None)? };

        // Create RTV for backbuffer
        let backbuffer: ID3D11Texture2D = unsafe { swapchain.GetBuffer(0)? };
        let mut rtv: Option<ID3D11RenderTargetView> = None;
        unsafe {
            self.device
                .CreateRenderTargetView(&backbuffer, None, Some(&mut rtv))?;
        }

        self.swapchain = Some(swapchain);
        self.backbuffer_rtv = rtv;

        info!("Swapchain created successfully");

        Ok(())
    }

    /// Create shared texture for streaming tools
    fn create_shared_texture(&mut self) -> Result<()> {
        info!("Creating shared texture for streaming");

        let desc = D3D11_TEXTURE2D_DESC {
            Width: self.config.width,
            Height: self.config.height,
            MipLevels: 1,
            ArraySize: 1,
            Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: (D3D11_BIND_SHADER_RESOURCE.0 | D3D11_BIND_RENDER_TARGET.0) as u32,
            CPUAccessFlags: Default::default(),
            MiscFlags: (D3D11_RESOURCE_MISC_SHARED.0 | D3D11_RESOURCE_MISC_SHARED_NTHANDLE.0)
                as u32,
        };

        let mut texture: Option<ID3D11Texture2D> = None;
        unsafe {
            self.device
                .CreateTexture2D(&desc, None, Some(&mut texture))?;
        }

        let texture = texture.ok_or_else(|| anyhow!("Failed to create shared texture"))?;

        // Get shared handle
        let dxgi_resource: windows::Win32::Graphics::Dxgi::IDXGIResource1 = texture.cast()?;
        let handle = unsafe {
            dxgi_resource.CreateSharedHandle(
                None,
                windows::Win32::Storage::FileSystem::FILE_GENERIC_READ.0
                    | windows::Win32::Storage::FileSystem::FILE_GENERIC_WRITE.0,
                None,
            )?
        };

        info!("Shared texture created with handle: {:?}", handle);

        self.shared_texture = Some(texture);
        self.shared_handle = Some(handle);

        Ok(())
    }

    /// Create named event for frame signaling
    fn create_frame_event(&mut self, name: &str) -> Result<()> {
        let name_wide: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();

        let event = unsafe { CreateEventW(None, false, false, PCWSTR(name_wide.as_ptr()))? };

        info!("Frame event created: {} ({:?})", name, event);

        self.frame_event = Some(event);

        Ok(())
    }

    /// Present a frame from the renderer's texture.
    /// 
    /// This copies the source texture to the swapchain backbuffer and/or shared texture,
    /// then presents and signals the frame event.
    pub fn present(&mut self, source_texture: &ID3D11Texture2D) -> Result<()> {
        debug!("Presenting frame");

        // Copy to swapchain backbuffer if in windowed/dual mode
        if let Some(ref swapchain) = self.swapchain {
            let backbuffer: ID3D11Texture2D = unsafe { swapchain.GetBuffer(0)? };

            unsafe {
                self.context.CopyResource(&backbuffer, source_texture);
            }

            // Present with vsync
            let sync_interval = if self.config.vsync { 1 } else { 0 };
            unsafe {
                swapchain.Present(sync_interval, DXGI_PRESENT(0))?;
            }
        }

        // Copy to shared texture if in headless/dual mode
        if let Some(ref shared_texture) = self.shared_texture {
            unsafe {
                self.context.CopyResource(shared_texture, source_texture);
            }
        }

        // Signal frame event
        if let Some(event) = self.frame_event {
            unsafe {
                let _ = SetEvent(event);
            }
        }

        Ok(())
    }

    /// Present using a specific subregion of the source texture
    pub fn present_region(
        &mut self,
        source_texture: &ID3D11Texture2D,
        src_x: u32,
        src_y: u32,
        width: u32,
        height: u32,
    ) -> Result<()> {
        let src_box = D3D11_BOX {
            left: src_x,
            top: src_y,
            front: 0,
            right: src_x + width,
            bottom: src_y + height,
            back: 1,
        };

        // Copy to swapchain backbuffer if in windowed/dual mode
        if let Some(ref swapchain) = self.swapchain {
            let backbuffer: ID3D11Texture2D = unsafe { swapchain.GetBuffer(0)? };

            unsafe {
                self.context
                    .CopySubresourceRegion(&backbuffer, 0, 0, 0, 0, source_texture, 0, Some(&src_box));
            }

            // Present with vsync
            let sync_interval = if self.config.vsync { 1 } else { 0 };
            unsafe {
                swapchain.Present(sync_interval, DXGI_PRESENT(0))?;
            }
        }

        // Copy to shared texture if in headless/dual mode
        if let Some(ref shared_texture) = self.shared_texture {
            unsafe {
                self.context
                    .CopySubresourceRegion(shared_texture, 0, 0, 0, 0, source_texture, 0, Some(&src_box));
            }
        }

        // Signal frame event
        if let Some(event) = self.frame_event {
            unsafe {
                let _ = SetEvent(event);
            }
        }

        Ok(())
    }

    /// Resize the presentation surface.
    pub fn resize(&mut self, width: u32, height: u32) -> Result<()> {
        if width == self.config.width && height == self.config.height {
            return Ok(());
        }

        info!("Resizing presentation to {}x{}", width, height);

        self.config.width = width;
        self.config.height = height;

        // Release old resources
        self.backbuffer_rtv = None;

        // Resize swapchain if exists
        if let Some(ref swapchain) = self.swapchain {
            unsafe {
                swapchain.ResizeBuffers(
                    2,
                    width,
                    height,
                    DXGI_FORMAT_R8G8B8A8_UNORM,
                    0,
                )?;
            }

            // Recreate RTV
            let backbuffer: ID3D11Texture2D = unsafe { swapchain.GetBuffer(0)? };
            let mut rtv: Option<ID3D11RenderTargetView> = None;
            unsafe {
                self.device
                    .CreateRenderTargetView(&backbuffer, None, Some(&mut rtv))?;
            }
            self.backbuffer_rtv = rtv;
        }

        // Recreate shared texture if exists
        if self.shared_texture.is_some() {
            self.shared_texture = None;
            self.shared_handle = None;
            self.create_shared_texture()?;
        }

        Ok(())
    }

    /// Process window messages (call this periodically)
    pub fn process_messages(&mut self) -> bool {
        if self.hwnd.is_none() {
            return true;
        }

        let mut msg = MSG::default();
        while unsafe { PeekMessageW(&mut msg, None, 0, 0, PM_REMOVE) }.as_bool() {
            if msg.message == windows::Win32::UI::WindowsAndMessaging::WM_QUIT {
                self.shutdown.store(true, Ordering::SeqCst);
                return false;
            }

            unsafe {
                let _ = TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        !self.shutdown.load(Ordering::SeqCst)
    }

    /// Check if shutdown was requested
    pub fn should_shutdown(&self) -> bool {
        self.shutdown.load(Ordering::SeqCst)
    }

    /// Get current dimensions.
    pub fn dimensions(&self) -> (u32, u32) {
        (self.config.width, self.config.height)
    }

    /// Get the presentation mode.
    #[allow(dead_code)]
    pub fn mode(&self) -> PresentationMode {
        self.config.mode
    }

    /// Check if vsync is enabled.
    #[allow(dead_code)]
    pub fn vsync(&self) -> bool {
        self.config.vsync
    }

    /// Get the shared texture handle (for streaming tools)
    pub fn shared_handle(&self) -> Option<windows::Win32::Foundation::HANDLE> {
        self.shared_handle
    }

    /// Get reference to the backbuffer RTV
    pub fn backbuffer_rtv(&self) -> Option<&ID3D11RenderTargetView> {
        self.backbuffer_rtv.as_ref()
    }

    /// Get the window handle
    pub fn hwnd(&self) -> Option<HWND> {
        self.hwnd
    }

    /// Get reference to shared texture
    pub fn shared_texture(&self) -> Option<&ID3D11Texture2D> {
        self.shared_texture.as_ref()
    }
}

impl Drop for PresentationPipeline {
    fn drop(&mut self) {
        info!("Destroying presentation pipeline");

        // Clean up resources
        self.backbuffer_rtv = None;
        self.swapchain = None;
        self.shared_texture = None;

        // Close handles
        if let Some(handle) = self.shared_handle.take() {
            unsafe {
                let _ = windows::Win32::Foundation::CloseHandle(handle);
            }
        }

        if let Some(event) = self.frame_event.take() {
            unsafe {
                let _ = windows::Win32::Foundation::CloseHandle(event);
            }
        }

        // Destroy window
        if let Some(hwnd) = self.hwnd.take() {
            unsafe {
                let _ = DestroyWindow(hwnd);
            }
        }
    }
}

/// Window procedure for handling window messages
extern "system" fn window_proc(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    match msg {
        WM_CLOSE => {
            unsafe {
                PostQuitMessage(0);
            }
            LRESULT(0)
        }
        WM_DESTROY => {
            unsafe {
                PostQuitMessage(0);
            }
            LRESULT(0)
        }
        WM_ERASEBKGND => {
            // Don't erase background - we'll paint over it
            LRESULT(1)
        }
        WM_PAINT => {
            // Just validate the window
            unsafe {
                let _ = windows::Win32::Graphics::Gdi::ValidateRect(hwnd, None);
            }
            LRESULT(0)
        }
        WM_SIZE => {
            // Handle resize if needed
            // The main loop should call resize() based on window size changes
            LRESULT(0)
        }
        _ => unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_presentation_config_default() {
        let config = PresentationConfig::default();
        assert_eq!(config.width, 1920);
        assert_eq!(config.height, 1080);
        assert!(config.vsync);
        assert_eq!(config.mode, PresentationMode::Windowed);
    }
}
