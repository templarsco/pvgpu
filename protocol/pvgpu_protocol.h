/*
 * PVGPU Protocol Definitions
 * 
 * Shared header file for communication between:
 * - QEMU pvgpu device (C)
 * - Windows WDDM KMD/UMD drivers (C/C++)
 * - Host backend service (Rust via bindgen)
 * 
 * This file defines the binary protocol for guest↔host GPU virtualization.
 */

#ifndef PVGPU_PROTOCOL_H
#define PVGPU_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * =============================================================================
 * Protocol Constants
 * =============================================================================
 */

#define PVGPU_MAGIC             0x50564750  /* "PVGP" in little-endian */
#define PVGPU_VERSION_MAJOR     1
#define PVGPU_VERSION_MINOR     0
#define PVGPU_VERSION           ((PVGPU_VERSION_MAJOR << 16) | PVGPU_VERSION_MINOR)

/* Default sizes */
#define PVGPU_CONTROL_REGION_SIZE   0x1000      /* 4KB */
#define PVGPU_COMMAND_RING_SIZE     0x1000000   /* 16MB */
#define PVGPU_DEFAULT_SHMEM_SIZE    0x10000000  /* 256MB */

/* BAR definitions */
#define PVGPU_BAR0_SIZE             0x1000      /* 4KB - Config registers */
#define PVGPU_BAR2_SIZE_DEFAULT     0x10000000  /* 256MB - Shared memory */

/* PCI IDs (use subsystem for customization) */
#define PVGPU_VENDOR_ID             0x1AF4      /* Red Hat / virtio vendor space */
#define PVGPU_DEVICE_ID             0x10F0      /* Custom device ID for pvgpu */
#define PVGPU_SUBSYSTEM_VENDOR_ID   0x1AF4
#define PVGPU_SUBSYSTEM_ID          0x0001
#define PVGPU_REVISION              0x01
#define PVGPU_PCI_CLASS             0x030200    /* VGA compatible 3D controller */

/*
 * =============================================================================
 * BAR0 Register Offsets (Config/Control Registers)
 * =============================================================================
 */

#define PVGPU_REG_VERSION           0x00    /* R:  Protocol version */
#define PVGPU_REG_FEATURES          0x04    /* R:  Supported features bitmap (low 32) */
#define PVGPU_REG_FEATURES_HI       0x08    /* R:  Supported features bitmap (high 32) */
#define PVGPU_REG_STATUS            0x0C    /* RW: Device status */
#define PVGPU_REG_DOORBELL          0x10    /* W:  Doorbell (notify host of new commands) */
#define PVGPU_REG_IRQ_STATUS        0x14    /* RW: IRQ status (write 1 to clear) */
#define PVGPU_REG_IRQ_MASK          0x18    /* RW: IRQ mask */
#define PVGPU_REG_SHMEM_SIZE        0x1C    /* R:  Shared memory size in bytes */
#define PVGPU_REG_RING_SIZE         0x20    /* R:  Command ring size in bytes */
#define PVGPU_REG_RESET             0x24    /* W:  Write 1 to reset device */

/* Status register bits */
#define PVGPU_STATUS_READY          (1 << 0)
#define PVGPU_STATUS_ERROR          (1 << 1)
#define PVGPU_STATUS_BACKEND_CONN   (1 << 2)

/* IRQ bits */
#define PVGPU_IRQ_FENCE_COMPLETE    (1 << 0)
#define PVGPU_IRQ_ERROR             (1 << 1)

/*
 * =============================================================================
 * Feature Flags
 * =============================================================================
 */

#define PVGPU_FEATURE_D3D11         (1ULL << 0)     /* D3D11 support */
#define PVGPU_FEATURE_D3D12         (1ULL << 1)     /* D3D12 support (future) */
#define PVGPU_FEATURE_COMPUTE       (1ULL << 2)     /* Compute shaders */
#define PVGPU_FEATURE_GEOMETRY      (1ULL << 3)     /* Geometry shaders */
#define PVGPU_FEATURE_TESSELLATION  (1ULL << 4)     /* Hull/Domain shaders */
#define PVGPU_FEATURE_MSAA          (1ULL << 5)     /* Multi-sample anti-aliasing */
#define PVGPU_FEATURE_HDR           (1ULL << 6)     /* HDR output (future) */
#define PVGPU_FEATURE_VSYNC         (1ULL << 7)     /* VSync support */
#define PVGPU_FEATURE_TRIPLE_BUFFER (1ULL << 8)     /* Triple buffering */

/* MVP features */
#define PVGPU_FEATURES_MVP          (PVGPU_FEATURE_D3D11 | PVGPU_FEATURE_COMPUTE | \
                                     PVGPU_FEATURE_GEOMETRY | PVGPU_FEATURE_TESSELLATION | \
                                     PVGPU_FEATURE_VSYNC)

/*
 * =============================================================================
 * Control Region Structure (at offset 0 of shared memory)
 * =============================================================================
 */

typedef struct PvgpuControlRegion {
    /* 0x000 */ uint32_t magic;                 /* Must be PVGPU_MAGIC */
    /* 0x004 */ uint32_t version;               /* Protocol version */
    /* 0x008 */ uint64_t features;              /* Negotiated feature bitmap */
    
    /* Ring configuration */
    /* 0x010 */ uint32_t ring_offset;           /* Command ring offset from shmem base */
    /* 0x014 */ uint32_t ring_size;             /* Command ring size in bytes */
    /* 0x018 */ uint32_t heap_offset;           /* Resource heap offset from shmem base */
    /* 0x01C */ uint32_t heap_size;             /* Resource heap size in bytes */
    
    /* Producer-consumer pointers (each on its own 64-byte cache line to prevent false sharing) */
    /* 0x020 */ volatile uint64_t producer_ptr; /* Written by guest */
    /*       */ uint8_t _pad_producer[56];      /* Pad to cache line boundary */
    /* 0x060 */ volatile uint64_t consumer_ptr; /* Written by host */
    /*       */ uint8_t _pad_consumer[56];      /* Pad to cache line boundary */
    
    /* Fence synchronization (each on its own cache line) */
    /* 0x0A0 */ volatile uint64_t guest_fence_request;    /* Latest fence requested by guest */
    /*       */ uint8_t _pad_guest_fence[56];              /* Pad to cache line boundary */
    /* 0x0E0 */ volatile uint64_t host_fence_completed;   /* Latest fence completed by host */
    /*       */ uint8_t _pad_host_fence[56];               /* Pad to cache line boundary */
    
    /* Status and error reporting */
    /* 0x120 */ volatile uint32_t status;       /* Device status flags */
    /* 0x124 */ volatile uint32_t error_code;   /* Last error code */
    /* 0x128 */ volatile uint32_t error_data;   /* Additional error info */
    /* 0x12C */ uint32_t reserved1;
    
    /* Display configuration */
    /* 0x130 */ uint32_t display_width;         /* Current display width */
    /* 0x134 */ uint32_t display_height;        /* Current display height */
    /* 0x138 */ uint32_t display_refresh;       /* Refresh rate in Hz */
    /* 0x13C */ uint32_t display_format;        /* DXGI_FORMAT value */
    
    /* Reserved for future use */
    /* 0x140 */ uint8_t reserved[0xEC0];        /* Pad to 4KB total */
} PvgpuControlRegion;

_Static_assert(sizeof(PvgpuControlRegion) == PVGPU_CONTROL_REGION_SIZE, 
               "Control region must be exactly 4KB");

/*
 * =============================================================================
 * Command Header (16 bytes)
 * =============================================================================
 */

typedef struct PvgpuCommandHeader {
    uint32_t command_type;      /* Command type (see PVGPU_CMD_*) */
    uint32_t command_size;      /* Total size including header */
    uint32_t resource_id;       /* Resource ID (if applicable) */
    uint32_t flags;             /* Command-specific flags */
} PvgpuCommandHeader;

#define PVGPU_CMD_HEADER_SIZE   sizeof(PvgpuCommandHeader)

/* Command flags */
#define PVGPU_CMD_FLAG_SYNC         (1 << 0)    /* Wait for completion */
#define PVGPU_CMD_FLAG_NO_FENCE     (1 << 1)    /* Don't signal fence */

/*
 * =============================================================================
 * Command Types
 * =============================================================================
 */

/* Resource commands: 0x0001 - 0x00FF */
#define PVGPU_CMD_CREATE_RESOURCE       0x0001
#define PVGPU_CMD_DESTROY_RESOURCE      0x0002
#define PVGPU_CMD_MAP_RESOURCE          0x0003
#define PVGPU_CMD_UNMAP_RESOURCE        0x0004
#define PVGPU_CMD_UPDATE_RESOURCE       0x0005
#define PVGPU_CMD_COPY_RESOURCE         0x0006
#define PVGPU_CMD_OPEN_RESOURCE         0x0007

/* State object creation commands: 0x0010 - 0x002F */
#define PVGPU_CMD_CREATE_BLEND_STATE        0x0010
#define PVGPU_CMD_DESTROY_BLEND_STATE       0x0011
#define PVGPU_CMD_CREATE_RASTERIZER_STATE   0x0012
#define PVGPU_CMD_DESTROY_RASTERIZER_STATE  0x0013
#define PVGPU_CMD_CREATE_DEPTH_STENCIL_STATE 0x0014
#define PVGPU_CMD_DESTROY_DEPTH_STENCIL_STATE 0x0015
#define PVGPU_CMD_CREATE_SAMPLER            0x0016
#define PVGPU_CMD_DESTROY_SAMPLER           0x0017
#define PVGPU_CMD_CREATE_INPUT_LAYOUT       0x0018
#define PVGPU_CMD_DESTROY_INPUT_LAYOUT      0x0019

/* View creation commands: 0x0020 - 0x002F */
#define PVGPU_CMD_CREATE_RENDER_TARGET_VIEW     0x0020
#define PVGPU_CMD_DESTROY_RENDER_TARGET_VIEW    0x0021
#define PVGPU_CMD_CREATE_DEPTH_STENCIL_VIEW     0x0022
#define PVGPU_CMD_DESTROY_DEPTH_STENCIL_VIEW    0x0023
#define PVGPU_CMD_CREATE_SHADER_RESOURCE_VIEW   0x0024
#define PVGPU_CMD_DESTROY_SHADER_RESOURCE_VIEW  0x0025
#define PVGPU_CMD_CREATE_UNORDERED_ACCESS_VIEW  0x0026
#define PVGPU_CMD_DESTROY_UNORDERED_ACCESS_VIEW 0x0027

/* Shader creation commands: 0x0030 - 0x003F */
#define PVGPU_CMD_CREATE_SHADER             0x0030
#define PVGPU_CMD_DESTROY_SHADER            0x0031

/* State commands: 0x0100 - 0x01FF */
#define PVGPU_CMD_SET_RENDER_TARGET     0x0101
#define PVGPU_CMD_SET_VIEWPORT          0x0102
#define PVGPU_CMD_SET_SCISSOR           0x0103
#define PVGPU_CMD_SET_BLEND_STATE       0x0104
#define PVGPU_CMD_SET_RASTERIZER_STATE  0x0105
#define PVGPU_CMD_SET_DEPTH_STENCIL     0x0106
#define PVGPU_CMD_SET_SHADER            0x0107
#define PVGPU_CMD_SET_SAMPLER           0x0108
#define PVGPU_CMD_SET_CONSTANT_BUFFER   0x0109
#define PVGPU_CMD_SET_VERTEX_BUFFER     0x010A
#define PVGPU_CMD_SET_INDEX_BUFFER      0x010B
#define PVGPU_CMD_SET_INPUT_LAYOUT      0x010C
#define PVGPU_CMD_SET_PRIMITIVE_TOPOLOGY 0x010D
#define PVGPU_CMD_SET_SHADER_RESOURCE   0x010E

/* Draw commands: 0x0200 - 0x02FF */
#define PVGPU_CMD_DRAW                  0x0201
#define PVGPU_CMD_DRAW_INDEXED          0x0202
#define PVGPU_CMD_DRAW_INSTANCED        0x0203
#define PVGPU_CMD_DRAW_INDEXED_INSTANCED 0x0204
#define PVGPU_CMD_DISPATCH              0x0205
#define PVGPU_CMD_CLEAR_RENDER_TARGET   0x0206
#define PVGPU_CMD_CLEAR_DEPTH_STENCIL   0x0207

/* Sync commands: 0x0300 - 0x03FF */
#define PVGPU_CMD_FENCE                 0x0301
#define PVGPU_CMD_PRESENT               0x0302
#define PVGPU_CMD_FLUSH                 0x0303
#define PVGPU_CMD_WAIT_FENCE            0x0304
#define PVGPU_CMD_RESIZE_BUFFERS        0x0305

/*
 * =============================================================================
 * Resource Types and Formats
 * =============================================================================
 */

typedef enum PvgpuResourceType {
    PVGPU_RESOURCE_TEXTURE_1D       = 1,
    PVGPU_RESOURCE_TEXTURE_2D       = 2,
    PVGPU_RESOURCE_TEXTURE_3D       = 3,
    PVGPU_RESOURCE_BUFFER           = 4,
    PVGPU_RESOURCE_VERTEX_SHADER    = 5,
    PVGPU_RESOURCE_PIXEL_SHADER     = 6,
    PVGPU_RESOURCE_GEOMETRY_SHADER  = 7,
    PVGPU_RESOURCE_HULL_SHADER      = 8,
    PVGPU_RESOURCE_DOMAIN_SHADER    = 9,
    PVGPU_RESOURCE_COMPUTE_SHADER   = 10,
    PVGPU_RESOURCE_INPUT_LAYOUT     = 11,
    PVGPU_RESOURCE_BLEND_STATE      = 12,
    PVGPU_RESOURCE_RASTERIZER_STATE = 13,
    PVGPU_RESOURCE_DEPTH_STENCIL_STATE = 14,
    PVGPU_RESOURCE_SAMPLER_STATE    = 15,
    PVGPU_RESOURCE_RENDER_TARGET_VIEW = 16,
    PVGPU_RESOURCE_DEPTH_STENCIL_VIEW = 17,
    PVGPU_RESOURCE_SHADER_RESOURCE_VIEW = 18,
    PVGPU_RESOURCE_UNORDERED_ACCESS_VIEW = 19,
} PvgpuResourceType;

/* Buffer bind flags (matching D3D11_BIND_FLAG) */
#define PVGPU_BIND_VERTEX_BUFFER        (1 << 0)
#define PVGPU_BIND_INDEX_BUFFER         (1 << 1)
#define PVGPU_BIND_CONSTANT_BUFFER      (1 << 2)
#define PVGPU_BIND_SHADER_RESOURCE      (1 << 3)
#define PVGPU_BIND_RENDER_TARGET        (1 << 4)
#define PVGPU_BIND_DEPTH_STENCIL        (1 << 5)
#define PVGPU_BIND_UNORDERED_ACCESS     (1 << 6)

/* Shader stages */
typedef enum PvgpuShaderStage {
    PVGPU_STAGE_VERTEX      = 0,
    PVGPU_STAGE_PIXEL       = 1,
    PVGPU_STAGE_GEOMETRY    = 2,
    PVGPU_STAGE_HULL        = 3,
    PVGPU_STAGE_DOMAIN      = 4,
    PVGPU_STAGE_COMPUTE     = 5,
    PVGPU_STAGE_COUNT       = 6,
} PvgpuShaderStage;

/*
 * =============================================================================
 * Command Payloads
 * =============================================================================
 */

/* CMD_CREATE_RESOURCE payload */
typedef struct PvgpuCmdCreateResource {
    PvgpuCommandHeader header;
    uint32_t resource_type;         /* PvgpuResourceType */
    uint32_t format;                /* DXGI_FORMAT */
    uint32_t width;                 /* Width (textures) or size (buffers) */
    uint32_t height;                /* Height (textures) */
    uint32_t depth;                 /* Depth (3D textures) or array size */
    uint32_t mip_levels;            /* Mipmap levels */
    uint32_t sample_count;          /* MSAA sample count */
    uint32_t sample_quality;        /* MSAA quality */
    uint32_t bind_flags;            /* PVGPU_BIND_* flags */
    uint32_t misc_flags;            /* Misc flags */
    uint32_t heap_offset;           /* Offset in resource heap (for initial data) */
    uint32_t data_size;             /* Size of initial data */
    /* For shaders: bytecode follows in heap at heap_offset */
} PvgpuCmdCreateResource;

/* CMD_DESTROY_RESOURCE payload */
typedef struct PvgpuCmdDestroyResource {
    PvgpuCommandHeader header;
    /* resource_id in header specifies which resource */
} PvgpuCmdDestroyResource;

/* CMD_OPEN_RESOURCE payload - opens a shared resource by global handle */
typedef struct PvgpuCmdOpenResource {
    PvgpuCommandHeader header;
    uint32_t shared_handle;         /* Global shared resource handle (NT or KMT) */
    uint32_t resource_type;         /* PvgpuResourceType */
    uint32_t format;                /* DXGI_FORMAT */
    uint32_t width;
    uint32_t height;
    uint32_t bind_flags;            /* PVGPU_BIND_* flags */
    uint32_t misc_flags;
} PvgpuCmdOpenResource;

/* CMD_MAP_RESOURCE payload */
typedef struct PvgpuCmdMapResource {
    PvgpuCommandHeader header;
    uint32_t resource_id;           /* Resource to map (in header) */
    uint32_t subresource;           /* Subresource index */
    uint32_t map_type;              /* Map type (read, write, etc.) */
    uint32_t map_flags;             /* Map flags */
    uint32_t heap_offset;           /* Where in heap to map data */
    uint32_t reserved[3];
} PvgpuCmdMapResource;

/* Map types (matches D3D11_MAP) */
#define PVGPU_MAP_READ              1
#define PVGPU_MAP_WRITE             2
#define PVGPU_MAP_READ_WRITE        3
#define PVGPU_MAP_WRITE_DISCARD     4
#define PVGPU_MAP_WRITE_NO_OVERWRITE 5

/* CMD_UNMAP_RESOURCE payload */
typedef struct PvgpuCmdUnmapResource {
    PvgpuCommandHeader header;
    uint32_t resource_id;           /* Resource to unmap (in header) */
    uint32_t subresource;           /* Subresource index */
    uint32_t heap_offset;           /* Where the data was mapped */
    uint32_t data_size;             /* Size of data to copy back (for write maps) */
} PvgpuCmdUnmapResource;

/* CMD_UPDATE_RESOURCE payload */
typedef struct PvgpuCmdUpdateResource {
    PvgpuCommandHeader header;
    uint32_t resource_id;           /* Resource to update (in header) */
    uint32_t subresource;           /* Subresource index */
    uint32_t heap_offset;           /* Source data offset in heap */
    uint32_t data_size;             /* Size of data in heap */
    uint32_t dst_x, dst_y, dst_z;   /* Destination offset */
    uint32_t width, height, depth;  /* Update region size (0 = full resource) */
    uint32_t row_pitch;             /* Source row pitch */
    uint32_t depth_pitch;           /* Source depth pitch */
} PvgpuCmdUpdateResource;

/* CMD_SET_RENDER_TARGET payload */
typedef struct PvgpuCmdSetRenderTarget {
    PvgpuCommandHeader header;
    uint32_t num_rtvs;              /* Number of render targets */
    uint32_t dsv_id;                /* Depth stencil view ID (0 = none) */
    uint32_t rtv_ids[8];            /* Render target view IDs */
} PvgpuCmdSetRenderTarget;

/* CMD_SET_VIEWPORT payload */
typedef struct PvgpuCmdSetViewport {
    PvgpuCommandHeader header;
    uint32_t num_viewports;
    struct {
        float x, y;
        float width, height;
        float min_depth, max_depth;
    } viewports[16];
} PvgpuCmdSetViewport;

/* CMD_SET_SCISSOR payload */
typedef struct PvgpuCmdSetScissor {
    PvgpuCommandHeader header;
    uint32_t num_rects;
    struct {
        int32_t left, top, right, bottom;
    } rects[16];
} PvgpuCmdSetScissor;

/* CMD_SET_SHADER payload */
typedef struct PvgpuCmdSetShader {
    PvgpuCommandHeader header;
    uint32_t stage;                 /* PvgpuShaderStage */
    uint32_t shader_id;             /* Shader resource ID (0 = unbind) */
} PvgpuCmdSetShader;

typedef struct PvgpuCmdCreateShader {
    PvgpuCommandHeader header;
    uint32_t shader_id;
    uint32_t shader_type;     /* ShaderStage enum value */
    uint32_t bytecode_size;
    uint32_t bytecode_offset; /* Offset into heap where bytecode data resides */
} PvgpuCmdCreateShader;

typedef struct PvgpuCmdDestroyShader {
    PvgpuCommandHeader header;
    uint32_t shader_id;
    uint32_t reserved[3];
} PvgpuCmdDestroyShader;

/* CMD_SET_CONSTANT_BUFFER payload */
typedef struct PvgpuCmdSetConstantBuffer {
    PvgpuCommandHeader header;
    uint32_t stage;                 /* PvgpuShaderStage */
    uint32_t slot;                  /* Constant buffer slot */
    uint32_t buffer_id;             /* Buffer resource ID (0 = unbind) */
    uint32_t offset;                /* Offset in constants (for dynamic CB) */
    uint32_t size;                  /* Size in constants */
} PvgpuCmdSetConstantBuffer;

/* CMD_SET_VERTEX_BUFFER payload */
typedef struct PvgpuCmdSetVertexBuffer {
    PvgpuCommandHeader header;
    uint32_t start_slot;
    uint32_t num_buffers;
    struct {
        uint32_t buffer_id;
        uint32_t stride;
        uint32_t offset;
    } buffers[16];
} PvgpuCmdSetVertexBuffer;

/* CMD_SET_INDEX_BUFFER payload */
typedef struct PvgpuCmdSetIndexBuffer {
    PvgpuCommandHeader header;
    uint32_t buffer_id;             /* Index buffer resource ID */
    uint32_t format;                /* DXGI_FORMAT (R16_UINT or R32_UINT) */
    uint32_t offset;                /* Offset in bytes */
    uint32_t reserved;
} PvgpuCmdSetIndexBuffer;

/* CMD_SET_PRIMITIVE_TOPOLOGY payload */
typedef struct PvgpuCmdSetPrimitiveTopology {
    PvgpuCommandHeader header;
    uint32_t topology;              /* D3D11_PRIMITIVE_TOPOLOGY */
    uint32_t reserved[3];
} PvgpuCmdSetPrimitiveTopology;

/* CMD_DRAW payload */
typedef struct PvgpuCmdDraw {
    PvgpuCommandHeader header;
    uint32_t vertex_count;
    uint32_t start_vertex;
    uint32_t reserved[2];
} PvgpuCmdDraw;

/* CMD_DRAW_INDEXED payload */
typedef struct PvgpuCmdDrawIndexed {
    PvgpuCommandHeader header;
    uint32_t index_count;
    uint32_t start_index;
    int32_t  base_vertex;
    uint32_t reserved;
} PvgpuCmdDrawIndexed;

/* CMD_DRAW_INSTANCED payload */
typedef struct PvgpuCmdDrawInstanced {
    PvgpuCommandHeader header;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t start_vertex;
    uint32_t start_instance;
} PvgpuCmdDrawInstanced;

/* CMD_DRAW_INDEXED_INSTANCED payload */
typedef struct PvgpuCmdDrawIndexedInstanced {
    PvgpuCommandHeader header;
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t start_index;
    int32_t  base_vertex;
    uint32_t start_instance;
    uint32_t reserved[3];
} PvgpuCmdDrawIndexedInstanced;

/* CMD_DISPATCH payload (compute shader) */
typedef struct PvgpuCmdDispatch {
    PvgpuCommandHeader header;
    uint32_t thread_group_x;
    uint32_t thread_group_y;
    uint32_t thread_group_z;
    uint32_t reserved;
} PvgpuCmdDispatch;

/* CMD_CLEAR_RENDER_TARGET payload */
typedef struct PvgpuCmdClearRenderTarget {
    PvgpuCommandHeader header;
    uint32_t rtv_id;
    float color[4];
} PvgpuCmdClearRenderTarget;

/* CMD_CLEAR_DEPTH_STENCIL payload */
typedef struct PvgpuCmdClearDepthStencil {
    PvgpuCommandHeader header;
    uint32_t dsv_id;
    uint32_t clear_flags;           /* D3D11_CLEAR_* flags */
    float depth;
    uint8_t stencil;
    uint8_t reserved[3];
} PvgpuCmdClearDepthStencil;

/* CMD_FENCE payload */
typedef struct PvgpuCmdFence {
    PvgpuCommandHeader header;
    uint64_t fence_value;           /* Fence value to signal */
} PvgpuCmdFence;

/* CMD_PRESENT payload */
typedef struct PvgpuCmdPresent {
    PvgpuCommandHeader header;
    uint32_t backbuffer_id;         /* Render target to present */
    uint32_t sync_interval;         /* VSync interval (0 = no vsync) */
    uint32_t flags;                 /* Present flags */
    uint32_t reserved;
} PvgpuCmdPresent;

/* CMD_RESIZE_BUFFERS payload */
typedef struct PvgpuCmdResizeBuffers {
    PvgpuCommandHeader header;
    uint32_t swapchain_id;          /* Swapchain to resize (0 = default) */
    uint32_t width;                 /* New width in pixels */
    uint32_t height;                /* New height in pixels */
    uint32_t format;                /* New format (DXGI_FORMAT, 0 = keep current) */
    uint32_t buffer_count;          /* New buffer count (0 = keep current) */
    uint32_t flags;                 /* Resize flags */
    uint32_t reserved[2];
} PvgpuCmdResizeBuffers;

/* CMD_SET_BLEND_STATE payload */
typedef struct PvgpuCmdSetBlendState {
    PvgpuCommandHeader header;
    uint32_t blend_state_id;        /* Blend state resource ID (0 = default) */
    float blend_factor[4];          /* Blend factor RGBA */
    uint32_t sample_mask;           /* Sample mask for multisample coverage */
    uint32_t reserved;
} PvgpuCmdSetBlendState;

/* CMD_SET_RASTERIZER_STATE payload */
typedef struct PvgpuCmdSetRasterizerState {
    PvgpuCommandHeader header;
    uint32_t rasterizer_state_id;   /* Rasterizer state resource ID (0 = default) */
    uint32_t reserved[3];
} PvgpuCmdSetRasterizerState;

/* CMD_SET_DEPTH_STENCIL payload */
typedef struct PvgpuCmdSetDepthStencilState {
    PvgpuCommandHeader header;
    uint32_t depth_stencil_state_id; /* Depth stencil state resource ID (0 = default) */
    uint32_t stencil_ref;            /* Stencil reference value */
    uint32_t reserved[2];
} PvgpuCmdSetDepthStencilState;

/* CMD_COPY_RESOURCE payload */
typedef struct PvgpuCmdCopyResource {
    PvgpuCommandHeader header;
    uint32_t dst_resource_id;       /* Destination resource ID */
    uint32_t src_resource_id;       /* Source resource ID */
    uint32_t reserved[2];
} PvgpuCmdCopyResource;

/* CMD_COPY_RESOURCE_REGION payload */
typedef struct PvgpuCmdCopyResourceRegion {
    PvgpuCommandHeader header;
    uint32_t dst_resource_id;       /* Destination resource ID */
    uint32_t dst_subresource;       /* Destination subresource index */
    uint32_t dst_x, dst_y, dst_z;   /* Destination offset */
    uint32_t src_resource_id;       /* Source resource ID */
    uint32_t src_subresource;       /* Source subresource index */
    uint32_t has_src_box;           /* Whether src_box is valid */
    struct {
        uint32_t left, top, front;
        uint32_t right, bottom, back;
    } src_box;
} PvgpuCmdCopyResourceRegion;

/*
 * =============================================================================
 * State Object Creation Payloads
 * =============================================================================
 */

/* CMD_CREATE_BLEND_STATE payload */
typedef struct PvgpuCmdCreateBlendState {
    PvgpuCommandHeader header;
    uint32_t state_id;              /* Assigned state object ID */
    uint32_t alpha_to_coverage;     /* Enable alpha-to-coverage */
    uint32_t independent_blend;     /* Enable independent blend per target */
    struct {
        uint32_t blend_enable;
        uint32_t src_blend;         /* D3D11_BLEND */
        uint32_t dest_blend;        /* D3D11_BLEND */
        uint32_t blend_op;          /* D3D11_BLEND_OP */
        uint32_t src_blend_alpha;   /* D3D11_BLEND */
        uint32_t dest_blend_alpha;  /* D3D11_BLEND */
        uint32_t blend_op_alpha;    /* D3D11_BLEND_OP */
        uint32_t render_target_write_mask; /* D3D11_COLOR_WRITE_ENABLE */
    } render_targets[8];
} PvgpuCmdCreateBlendState;

/* CMD_CREATE_RASTERIZER_STATE payload */
typedef struct PvgpuCmdCreateRasterizerState {
    PvgpuCommandHeader header;
    uint32_t state_id;              /* Assigned state object ID */
    uint32_t fill_mode;             /* D3D11_FILL_MODE */
    uint32_t cull_mode;             /* D3D11_CULL_MODE */
    uint32_t front_counter_clockwise;
    int32_t  depth_bias;
    float    depth_bias_clamp;
    float    slope_scaled_depth_bias;
    uint32_t depth_clip_enable;
    uint32_t scissor_enable;
    uint32_t multisample_enable;
    uint32_t antialiased_line_enable;
} PvgpuCmdCreateRasterizerState;

/* CMD_CREATE_DEPTH_STENCIL_STATE payload */
typedef struct PvgpuCmdCreateDepthStencilState {
    PvgpuCommandHeader header;
    uint32_t state_id;              /* Assigned state object ID */
    uint32_t depth_enable;
    uint32_t depth_write_mask;      /* D3D11_DEPTH_WRITE_MASK */
    uint32_t depth_func;            /* D3D11_COMPARISON_FUNC */
    uint32_t stencil_enable;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    struct {
        uint32_t stencil_fail_op;   /* D3D11_STENCIL_OP */
        uint32_t stencil_depth_fail_op;
        uint32_t stencil_pass_op;
        uint32_t stencil_func;      /* D3D11_COMPARISON_FUNC */
    } front_face;
    struct {
        uint32_t stencil_fail_op;
        uint32_t stencil_depth_fail_op;
        uint32_t stencil_pass_op;
        uint32_t stencil_func;
    } back_face;
} PvgpuCmdCreateDepthStencilState;

/* CMD_CREATE_SAMPLER payload */
typedef struct PvgpuCmdCreateSampler {
    PvgpuCommandHeader header;
    uint32_t sampler_id;            /* Assigned sampler ID */
    uint32_t filter;                /* D3D11_FILTER */
    uint32_t address_u;             /* D3D11_TEXTURE_ADDRESS_MODE */
    uint32_t address_v;
    uint32_t address_w;
    float    mip_lod_bias;
    uint32_t max_anisotropy;
    uint32_t comparison_func;       /* D3D11_COMPARISON_FUNC */
    float    border_color[4];
    float    min_lod;
    float    max_lod;
} PvgpuCmdCreateSampler;

/* CMD_CREATE_INPUT_LAYOUT payload */
typedef struct PvgpuCmdCreateInputLayout {
    PvgpuCommandHeader header;
    uint32_t layout_id;             /* Assigned layout ID */
    uint32_t num_elements;          /* Number of input elements */
    struct {
        uint32_t semantic_name_offset; /* Offset in shared memory for name string */
        uint32_t semantic_index;
        uint32_t format;            /* DXGI_FORMAT */
        uint32_t input_slot;
        uint32_t aligned_byte_offset;
        uint32_t input_slot_class;  /* D3D11_INPUT_CLASSIFICATION */
        uint32_t instance_data_step_rate;
    } elements[32];                 /* Max 32 input elements */
} PvgpuCmdCreateInputLayout;

/*
 * =============================================================================
 * View Creation Payloads
 * =============================================================================
 */

/* CMD_CREATE_RENDER_TARGET_VIEW payload */
typedef struct PvgpuCmdCreateRenderTargetView {
    PvgpuCommandHeader header;
    uint32_t view_id;               /* Assigned view ID */
    uint32_t resource_id;           /* Resource to create view of */
    uint32_t format;                /* DXGI_FORMAT */
    uint32_t view_dimension;        /* D3D11_RTV_DIMENSION */
    union {
        struct { uint32_t mip_slice; } texture1d;
        struct { uint32_t mip_slice; uint32_t first_array_slice; uint32_t array_size; } texture1d_array;
        struct { uint32_t mip_slice; } texture2d;
        struct { uint32_t mip_slice; uint32_t first_array_slice; uint32_t array_size; } texture2d_array;
        struct { uint32_t mip_slice; uint32_t first_w_slice; uint32_t w_size; } texture3d;
    } u;
} PvgpuCmdCreateRenderTargetView;

/* CMD_CREATE_DEPTH_STENCIL_VIEW payload */
typedef struct PvgpuCmdCreateDepthStencilView {
    PvgpuCommandHeader header;
    uint32_t view_id;               /* Assigned view ID */
    uint32_t resource_id;           /* Resource to create view of */
    uint32_t format;                /* DXGI_FORMAT */
    uint32_t view_dimension;        /* D3D11_DSV_DIMENSION */
    uint32_t flags;                 /* D3D11_DSV_FLAG */
    union {
        struct { uint32_t mip_slice; } texture1d;
        struct { uint32_t mip_slice; uint32_t first_array_slice; uint32_t array_size; } texture1d_array;
        struct { uint32_t mip_slice; } texture2d;
        struct { uint32_t mip_slice; uint32_t first_array_slice; uint32_t array_size; } texture2d_array;
    } u;
} PvgpuCmdCreateDepthStencilView;

/* CMD_CREATE_SHADER_RESOURCE_VIEW payload */
typedef struct PvgpuCmdCreateShaderResourceView {
    PvgpuCommandHeader header;
    uint32_t view_id;               /* Assigned view ID */
    uint32_t resource_id;           /* Resource to create view of */
    uint32_t format;                /* DXGI_FORMAT */
    uint32_t view_dimension;        /* D3D11_SRV_DIMENSION */
    union {
        struct { uint32_t most_detailed_mip; uint32_t mip_levels; } texture1d;
        struct { uint32_t most_detailed_mip; uint32_t mip_levels; uint32_t first_array_slice; uint32_t array_size; } texture1d_array;
        struct { uint32_t most_detailed_mip; uint32_t mip_levels; } texture2d;
        struct { uint32_t most_detailed_mip; uint32_t mip_levels; uint32_t first_array_slice; uint32_t array_size; } texture2d_array;
        struct { uint32_t most_detailed_mip; uint32_t mip_levels; } texture3d;
        struct { uint32_t most_detailed_mip; uint32_t mip_levels; } texturecube;
        struct { uint32_t first_element; uint32_t num_elements; } buffer;
    } u;
} PvgpuCmdCreateShaderResourceView;

/* CMD_SET_SHADER_RESOURCES payload */
typedef struct PvgpuCmdSetShaderResources {
    PvgpuCommandHeader header;
    uint32_t stage;                 /* PvgpuShaderStage */
    uint32_t start_slot;
    uint32_t num_views;
    uint32_t view_ids[128];         /* SRV IDs (0 = unbind) */
} PvgpuCmdSetShaderResources;

/* CMD_SET_SAMPLERS payload */
typedef struct PvgpuCmdSetSamplers {
    PvgpuCommandHeader header;
    uint32_t stage;                 /* PvgpuShaderStage */
    uint32_t start_slot;
    uint32_t num_samplers;
    uint32_t sampler_ids[16];       /* Sampler IDs (0 = unbind) */
} PvgpuCmdSetSamplers;

/*
 * =============================================================================
 * Error Codes
 * =============================================================================
 */

#define PVGPU_ERROR_SUCCESS             0x0000
#define PVGPU_ERROR_INVALID_COMMAND     0x0001
#define PVGPU_ERROR_RESOURCE_NOT_FOUND  0x0002
#define PVGPU_ERROR_OUT_OF_MEMORY       0x0003
#define PVGPU_ERROR_SHADER_COMPILE      0x0004
#define PVGPU_ERROR_DEVICE_LOST         0x0005
#define PVGPU_ERROR_INVALID_PARAMETER   0x0006
#define PVGPU_ERROR_UNSUPPORTED_FORMAT  0x0007
#define PVGPU_ERROR_BACKEND_DISCONNECTED 0x0008
#define PVGPU_ERROR_RING_FULL           0x0009
#define PVGPU_ERROR_TIMEOUT             0x000A
#define PVGPU_ERROR_HEAP_EXHAUSTED      0x000B  /* Resource heap is full */
#define PVGPU_ERROR_INTERNAL            0x000C  /* Internal backend error */
#define PVGPU_ERROR_UNKNOWN             0xFFFF

/*
 * =============================================================================
 * Device Status Flags (for PvgpuControlRegion.status)
 * =============================================================================
 */

#define PVGPU_STATUS_READY              (1 << 0)    /* Backend is ready */
#define PVGPU_STATUS_ERROR              (1 << 1)    /* Error occurred (check error_code) */
#define PVGPU_STATUS_DEVICE_LOST        (1 << 2)    /* D3D device lost, needs reset */
#define PVGPU_STATUS_BACKEND_BUSY       (1 << 3)    /* Backend is processing */
#define PVGPU_STATUS_RESIZING           (1 << 4)    /* Swapchain resize in progress */
#define PVGPU_STATUS_RECOVERY           (1 << 5)    /* Device recovery in progress */
#define PVGPU_STATUS_SHUTDOWN           (1 << 6)    /* Backend is shutting down */

/*
 * =============================================================================
 * UMD <-> KMD Escape Interface
 * =============================================================================
 * 
 * The UMD communicates with the KMD through DxgkDdiEscape.
 * This allows the UMD to:
 * - Allocate/free regions in the shared memory heap
 * - Get the shared memory base address for direct access
 * - Submit commands to the ring buffer
 * - Query device capabilities
 */

/* Escape codes */
#define PVGPU_ESCAPE_GET_SHMEM_INFO         0x0001  /* Get shared memory info */
#define PVGPU_ESCAPE_ALLOC_HEAP             0x0002  /* Allocate from heap */
#define PVGPU_ESCAPE_FREE_HEAP              0x0003  /* Free heap allocation */
#define PVGPU_ESCAPE_SUBMIT_COMMANDS        0x0004  /* Submit commands to ring */
#define PVGPU_ESCAPE_WAIT_FENCE             0x0005  /* Wait for fence completion */
#define PVGPU_ESCAPE_GET_CAPS               0x0006  /* Get device capabilities */
#define PVGPU_ESCAPE_RING_DOORBELL          0x0007  /* Ring the doorbell */
#define PVGPU_ESCAPE_SET_DISPLAY_MODE       0x0008  /* Notify display mode change */

/* Common escape header */
typedef struct PvgpuEscapeHeader {
    uint32_t escape_code;           /* PVGPU_ESCAPE_* */
    uint32_t status;                /* Return status (PVGPU_ERROR_*) */
} PvgpuEscapeHeader;

/* PVGPU_ESCAPE_GET_SHMEM_INFO input/output */
typedef struct PvgpuEscapeGetShmemInfo {
    PvgpuEscapeHeader header;
    /* Output */
    uint64_t shmem_base;            /* Virtual address of shared memory (for UMD) */
    uint32_t shmem_size;            /* Total shared memory size */
    uint32_t ring_offset;           /* Offset to command ring */
    uint32_t ring_size;             /* Command ring size */
    uint32_t heap_offset;           /* Offset to resource heap */
    uint32_t heap_size;             /* Resource heap size */
    uint64_t features;              /* Negotiated features */
} PvgpuEscapeGetShmemInfo;

/* PVGPU_ESCAPE_ALLOC_HEAP input/output */
typedef struct PvgpuEscapeAllocHeap {
    PvgpuEscapeHeader header;
    /* Input */
    uint32_t size;                  /* Requested size (must be 16-byte aligned) */
    uint32_t alignment;             /* Required alignment (power of 2) */
    /* Output */
    uint32_t offset;                /* Allocated offset in heap */
    uint32_t allocated_size;        /* Actual allocated size */
} PvgpuEscapeAllocHeap;

/* PVGPU_ESCAPE_FREE_HEAP input */
typedef struct PvgpuEscapeFreeHeap {
    PvgpuEscapeHeader header;
    /* Input */
    uint32_t offset;                /* Offset to free */
    uint32_t size;                  /* Size to free */
} PvgpuEscapeFreeHeap;

/* PVGPU_ESCAPE_SUBMIT_COMMANDS input */
typedef struct PvgpuEscapeSubmitCommands {
    PvgpuEscapeHeader header;
    /* Input */
    uint32_t command_offset;        /* Offset in heap where commands are staged */
    uint32_t command_size;          /* Total size of commands */
    uint64_t fence_value;           /* Fence value (0 = no fence) */
    /* Output */
    uint64_t producer_ptr;          /* Updated producer pointer */
} PvgpuEscapeSubmitCommands;

/* PVGPU_ESCAPE_WAIT_FENCE input */
typedef struct PvgpuEscapeWaitFence {
    PvgpuEscapeHeader header;
    /* Input */
    uint64_t fence_value;           /* Fence value to wait for */
    uint32_t timeout_ms;            /* Timeout in milliseconds (0 = infinite) */
    uint32_t reserved;
    /* Output */
    uint64_t completed_fence;       /* Current completed fence value */
} PvgpuEscapeWaitFence;

/* PVGPU_ESCAPE_GET_CAPS output */
typedef struct PvgpuEscapeGetCaps {
    PvgpuEscapeHeader header;
    /* Output */
    uint64_t features;              /* Supported features bitmap */
    uint32_t max_texture_size;      /* Maximum texture dimension */
    uint32_t max_render_targets;    /* Maximum simultaneous render targets */
    uint32_t max_vertex_streams;    /* Maximum vertex buffer streams */
    uint32_t max_constant_buffers;  /* Maximum constant buffers per stage */
    uint32_t display_width;         /* Current display width */
    uint32_t display_height;        /* Current display height */
    uint32_t display_refresh;       /* Current refresh rate */
    uint32_t reserved[5];
} PvgpuEscapeGetCaps;

/* PVGPU_ESCAPE_SET_DISPLAY_MODE input */
typedef struct PvgpuEscapeSetDisplayMode {
    PvgpuEscapeHeader header;
    /* Input */
    uint32_t width;                 /* New display width */
    uint32_t height;                /* New display height */
    uint32_t refresh_rate;          /* New refresh rate in Hz */
    uint32_t flags;                 /* Reserved for future use */
} PvgpuEscapeSetDisplayMode;

/*
 * =============================================================================
 * Utility Macros
 * =============================================================================
 */

/* Check if ring has space for N bytes */
#define PVGPU_RING_HAS_SPACE(ctrl, size) \
    (((ctrl)->producer_ptr - (ctrl)->consumer_ptr) + (size) <= (ctrl)->ring_size)

/* Get write pointer in ring */
#define PVGPU_RING_WRITE_PTR(base, ctrl) \
    ((uint8_t*)(base) + (ctrl)->ring_offset + ((ctrl)->producer_ptr % (ctrl)->ring_size))

/* Get read pointer in ring */
#define PVGPU_RING_READ_PTR(base, ctrl) \
    ((uint8_t*)(base) + (ctrl)->ring_offset + ((ctrl)->consumer_ptr % (ctrl)->ring_size))

/* Align size to 16-byte boundary */
#define PVGPU_ALIGN16(x) (((x) + 15) & ~15)

#ifdef __cplusplus
}
#endif

#endif /* PVGPU_PROTOCOL_H */
