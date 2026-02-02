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
    
    /* Producer-consumer pointers (atomics) */
    /* 0x020 */ volatile uint64_t producer_ptr; /* Written by guest */
    /* 0x028 */ volatile uint64_t consumer_ptr; /* Written by host */
    
    /* Fence synchronization */
    /* 0x030 */ volatile uint64_t guest_fence_request;    /* Latest fence requested by guest */
    /* 0x038 */ volatile uint64_t host_fence_completed;   /* Latest fence completed by host */
    
    /* Status and error reporting */
    /* 0x040 */ volatile uint32_t status;       /* Device status flags */
    /* 0x044 */ volatile uint32_t error_code;   /* Last error code */
    /* 0x048 */ volatile uint32_t error_data;   /* Additional error info */
    /* 0x04C */ uint32_t reserved1;
    
    /* Display configuration */
    /* 0x050 */ uint32_t display_width;         /* Current display width */
    /* 0x054 */ uint32_t display_height;        /* Current display height */
    /* 0x058 */ uint32_t display_refresh;       /* Refresh rate in Hz */
    /* 0x05C */ uint32_t display_format;        /* DXGI_FORMAT value */
    
    /* Reserved for future use */
    /* 0x060 */ uint8_t reserved[0xFA0];        /* Pad to 4KB total */
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

/* CMD_MAP_RESOURCE payload */
typedef struct PvgpuCmdMapResource {
    PvgpuCommandHeader header;
    uint32_t subresource;           /* Subresource index */
    uint32_t map_type;              /* Map type (read, write, etc.) */
    uint32_t heap_offset;           /* Where in heap to map data */
    uint32_t reserved;
} PvgpuCmdMapResource;

/* CMD_UPDATE_RESOURCE payload */
typedef struct PvgpuCmdUpdateResource {
    PvgpuCommandHeader header;
    uint32_t subresource;           /* Subresource index */
    uint32_t dst_x, dst_y, dst_z;   /* Destination offset */
    uint32_t width, height, depth;  /* Update region size */
    uint32_t heap_offset;           /* Source data offset in heap */
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
#define PVGPU_ERROR_UNKNOWN             0xFFFF

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
