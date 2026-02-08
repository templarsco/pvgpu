/*
 * PVGPU User-Mode Driver (UMD) - Header
 *
 * This is the D3D11 User-Mode Display Driver that runs in the application's
 * process space. It receives D3D11 API calls from the runtime and translates
 * them into pvgpu commands that are submitted to the KMD via command buffers.
 *
 * Copyright (c) SANSI-GROUP. All rights reserved.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */

#ifndef PVGPU_UMD_H
#define PVGPU_UMD_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d10umddi.h>
#include <d3d11_1.h>

#include "../../protocol/pvgpu_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Version and Capabilities
 * ============================================================================ */

#define PVGPU_UMD_VERSION_MAJOR     1
#define PVGPU_UMD_VERSION_MINOR     0
#define PVGPU_UMD_VERSION_BUILD     0

/* Maximum resources we can track per device */
#define PVGPU_UMD_MAX_RESOURCES         65536
#define PVGPU_UMD_MAX_RENDER_TARGETS    8
#define PVGPU_UMD_MAX_VERTEX_BUFFERS    32
#define PVGPU_UMD_MAX_SAMPLERS          16
#define PVGPU_UMD_MAX_SHADER_RESOURCES  128
#define PVGPU_UMD_MAX_CONSTANT_BUFFERS  14

/* Command staging buffer size.
 * Larger buffer = fewer ring flushes + fewer doorbell KMD escapes.
 * 256KB fits ~4000-8000 typical commands before a flush is needed. */
#define PVGPU_UMD_COMMAND_BUFFER_SIZE   (256 * 1024)

/* ============================================================================
 * Resource Tracking
 * ============================================================================ */

/* Shader types (mirrors PvgpuShaderStage from protocol) */
typedef enum PVGPU_SHADER_TYPE {
    PVGPU_SHADER_VERTEX   = 0,
    PVGPU_SHADER_PIXEL    = 1,
    PVGPU_SHADER_GEOMETRY = 2,
    PVGPU_SHADER_HULL     = 3,
    PVGPU_SHADER_DOMAIN   = 4,
    PVGPU_SHADER_COMPUTE  = 5,
} PVGPU_SHADER_TYPE;

/* Resource types we track */
typedef enum PVGPU_RESOURCE_TYPE {
    PVGPU_RESOURCE_TYPE_UNKNOWN = 0,
    PVGPU_RESOURCE_TYPE_BUFFER,
    PVGPU_RESOURCE_TYPE_TEXTURE1D,
    PVGPU_RESOURCE_TYPE_TEXTURE2D,
    PVGPU_RESOURCE_TYPE_TEXTURE3D,
    PVGPU_RESOURCE_TYPE_SHADER,
    PVGPU_RESOURCE_TYPE_SAMPLER,
    PVGPU_RESOURCE_TYPE_RENDER_TARGET_VIEW,
    PVGPU_RESOURCE_TYPE_DEPTH_STENCIL_VIEW,
    PVGPU_RESOURCE_TYPE_SHADER_RESOURCE_VIEW,
    PVGPU_RESOURCE_TYPE_UNORDERED_ACCESS_VIEW,
    PVGPU_RESOURCE_TYPE_INPUT_LAYOUT,
    PVGPU_RESOURCE_TYPE_BLEND_STATE,
    PVGPU_RESOURCE_TYPE_DEPTH_STENCIL_STATE,
    PVGPU_RESOURCE_TYPE_RASTERIZER_STATE,
} PVGPU_RESOURCE_TYPE;

/* UMD-side resource tracking structure */
typedef struct PVGPU_UMD_RESOURCE {
    PVGPU_RESOURCE_TYPE Type;
    UINT32              HostHandle;     /* Handle assigned by host backend */
    UINT32              Width;
    UINT32              Height;
    UINT32              Depth;
    UINT32              MipLevels;
    UINT32              ArraySize;
    DXGI_FORMAT         Format;
    UINT32              BindFlags;
    UINT32              MiscFlags;
    UINT32              ByteWidth;      /* For buffers */
    UINT32              StructureByteStride;
    BOOL                IsMapped;
    void*               MappedAddress;
    SIZE_T              MappedSize;
    BOOL                IsShared;       /* Opened via OpenResource */
    D3D10DDI_HRTRESOURCE hRTResource;  /* Runtime handle */
} PVGPU_UMD_RESOURCE;

/* Shader tracking */
typedef struct PVGPU_UMD_SHADER {
    PVGPU_SHADER_TYPE   Type;
    UINT32              HostHandle;
    SIZE_T              BytecodeSize;
    void*               Bytecode;       /* Keep a copy for debugging */
} PVGPU_UMD_SHADER;

/* Blend state tracking */
typedef struct PVGPU_UMD_BLEND_STATE {
    UINT32              HostHandle;
    BOOL                AlphaToCoverageEnable;
    BOOL                IndependentBlendEnable;
} PVGPU_UMD_BLEND_STATE;

/* Rasterizer state tracking */
typedef struct PVGPU_UMD_RASTERIZER_STATE {
    UINT32              HostHandle;
    UINT32              FillMode;
    UINT32              CullMode;
} PVGPU_UMD_RASTERIZER_STATE;

/* Depth stencil state tracking */
typedef struct PVGPU_UMD_DEPTH_STENCIL_STATE {
    UINT32              HostHandle;
    BOOL                DepthEnable;
    BOOL                StencilEnable;
} PVGPU_UMD_DEPTH_STENCIL_STATE;

/* Sampler state tracking */
typedef struct PVGPU_UMD_SAMPLER {
    UINT32              HostHandle;
    UINT32              Filter;
    UINT32              AddressU;
    UINT32              AddressV;
    UINT32              AddressW;
} PVGPU_UMD_SAMPLER;

/* Input layout tracking */
typedef struct PVGPU_UMD_INPUT_LAYOUT {
    UINT32              HostHandle;
    UINT32              NumElements;
} PVGPU_UMD_INPUT_LAYOUT;

/* Render target view tracking */
typedef struct PVGPU_UMD_RENDER_TARGET_VIEW {
    UINT32              HostHandle;
    UINT32              ResourceHandle; /* The underlying resource */
    DXGI_FORMAT         Format;
} PVGPU_UMD_RENDER_TARGET_VIEW;

/* Depth stencil view tracking */
typedef struct PVGPU_UMD_DEPTH_STENCIL_VIEW {
    UINT32              HostHandle;
    UINT32              ResourceHandle;
    DXGI_FORMAT         Format;
} PVGPU_UMD_DEPTH_STENCIL_VIEW;

/* Shader resource view tracking */
typedef struct PVGPU_UMD_SHADER_RESOURCE_VIEW {
    UINT32              HostHandle;
    UINT32              ResourceHandle;
    DXGI_FORMAT         Format;
} PVGPU_UMD_SHADER_RESOURCE_VIEW;

/* ============================================================================
 * Device Context
 * ============================================================================ */

/* Forward declarations */
struct PVGPU_UMD_DEVICE;
struct PVGPU_UMD_ADAPTER;

/* Per-device state */
typedef struct PVGPU_UMD_DEVICE {
    /* D3D10 DDI handles */
    D3D10DDI_HRTDEVICE              hRTDevice;
    D3D10DDI_HRTCORELAYER           hRTCoreLayer;
    CONST D3D10DDI_CORELAYER_DEVICECALLBACKS* pRTCallbacks;
    CONST D3DDDI_DEVICECALLBACKS*   pKTCallbacks;
    
    /* Adapter back-reference */
    struct PVGPU_UMD_ADAPTER*       pAdapter;
    
    /* Shared memory access (obtained via KMD escape) */
    void*                           pSharedMemory;      /* Base address of shared memory */
    SIZE_T                          SharedMemorySize;   /* Total shared memory size */
    PvgpuControlRegion*             pControlRegion;     /* Pointer to control region */
    UINT8*                          pRingBuffer;        /* Pointer to command ring */
    SIZE_T                          RingBufferSize;     /* Ring buffer size */
    UINT8*                          pHeap;              /* Pointer to resource heap */
    SIZE_T                          HeapSize;           /* Heap size */
    SIZE_T                          HeapOffset;         /* Heap start offset from shmem base */
    UINT64                          NegotiatedFeatures; /* Feature flags */
    BOOL                            SharedMemoryValid;  /* TRUE if shmem is mapped */
    BOOL                            BackendConnected;   /* TRUE if backend is responsive */
    
    /* Ring buffer producer tracking */
    UINT64                          LocalProducerPtr;   /* UMD's view of producer pointer */
    UINT64                          LastFenceSubmitted; /* Last fence value we submitted */
    UINT64                          NextFenceValue;     /* Next fence value to use */
    UINT64                          LastPresentFence;   /* Fence from previous present (for async double-buffer) */
    CRITICAL_SECTION                RingLock;           /* Lock for ring buffer access */
    
    /* Staging buffer for command batching (before ring submission) */
    UINT8*                          pStagingBuffer;     /* Local staging buffer */
    SIZE_T                          StagingBufferSize;  /* Staging buffer capacity */
    SIZE_T                          StagingOffset;      /* Current write offset */
    
    /* Resource tracking */
    PVGPU_UMD_RESOURCE*             pResources;
    UINT32                          ResourceCount;
    UINT32                          NextResourceHandle;
    CRITICAL_SECTION                ResourceLock;
    
    /* Current pipeline state (for tracking what's bound) */
    struct {
        UINT32 RenderTargets[PVGPU_UMD_MAX_RENDER_TARGETS];
        UINT32 RenderTargetCount;
        UINT32 DepthStencilView;
        
        UINT32 VertexShader;
        UINT32 PixelShader;
        UINT32 GeometryShader;
        UINT32 HullShader;
        UINT32 DomainShader;
        UINT32 ComputeShader;
        
        UINT32 VertexBuffers[PVGPU_UMD_MAX_VERTEX_BUFFERS];
        UINT32 VertexBufferStrides[PVGPU_UMD_MAX_VERTEX_BUFFERS];
        UINT32 VertexBufferOffsets[PVGPU_UMD_MAX_VERTEX_BUFFERS];
        UINT32 VertexBufferCount;
        
        UINT32 IndexBuffer;
        DXGI_FORMAT IndexBufferFormat;
        UINT32 IndexBufferOffset;
        
        UINT32 InputLayout;
        UINT32 PrimitiveTopology;
        
        D3D10_DDI_VIEWPORT Viewports[D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT];
        UINT32 ViewportCount;
        
        D3D10_DDI_RECT ScissorRects[D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT];
        UINT32 ScissorRectCount;
        
        UINT32 BlendState;
        FLOAT  BlendFactor[4];
        UINT32 SampleMask;
        
        UINT32 DepthStencilState;
        UINT32 StencilRef;
        
        UINT32 RasterizerState;
    } PipelineState;
    
    /* Statistics */
    UINT64 DrawCallCount;
    UINT64 CommandsSubmitted;
    
} PVGPU_UMD_DEVICE;

/* Per-adapter state (shared across devices) */
typedef struct PVGPU_UMD_ADAPTER {
    D3D10DDI_HRTADAPTER             hRTAdapter;
    CONST D3DDDI_ADAPTERCALLBACKS*  pAdapterCallbacks;
    
    /* Adapter capabilities */
    UINT32                          MaxTextureWidth;
    UINT32                          MaxTextureHeight;
    UINT32                          MaxTexture3DDepth;
    UINT32                          MaxTextureCubeSize;
    UINT32                          MaxPrimitiveCount;
    
    /* Feature support */
    BOOL                            SupportsCompute;
    BOOL                            SupportsTessellation;
    BOOL                            SupportsStreamOutput;
    D3D10_DDI_D3D10_OPTIONS_DATA    D3D10Options;
    D3D11DDI_3DPIPELINESUPPORT_CAPS PipelineCaps;
    
} PVGPU_UMD_ADAPTER;

/* ============================================================================
 * DDI Entry Points - Exported Functions
 * ============================================================================ */

/* Main DDI entry point - called by D3D runtime to get function table */
HRESULT APIENTRY OpenAdapter10_2(
    _Inout_ D3D10DDIARG_OPENADAPTER* pOpenData
);

/* ============================================================================
 * Adapter Functions
 * ============================================================================ */

SIZE_T APIENTRY PvgpuCalcPrivateDeviceSize(
    _In_ D3D10DDI_HADAPTER hAdapter,
    _In_ CONST D3D10DDIARG_CALCPRIVATEDEVICESIZE* pCalcPrivateDeviceSize
);

HRESULT APIENTRY PvgpuCreateDevice(
    _In_ D3D10DDI_HADAPTER hAdapter,
    _Inout_ D3D10DDIARG_CREATEDEVICE* pCreateData
);

HRESULT APIENTRY PvgpuCloseAdapter(
    _In_ D3D10DDI_HADAPTER hAdapter
);

HRESULT APIENTRY PvgpuGetCaps(
    _In_ D3D10DDI_HADAPTER hAdapter,
    _In_ CONST D3D10_2DDIARG_GETCAPS* pData
);

HRESULT APIENTRY PvgpuGetSupportedVersions(
    _In_ D3D10DDI_HADAPTER hAdapter,
    _Inout_ UINT32* puEntries,
    _Out_writes_opt_(*puEntries) UINT64* pSupportedDDIInterfaceVersions
);

/* ============================================================================
 * Device Functions
 * ============================================================================ */

void APIENTRY PvgpuDestroyDevice(
    _In_ D3D10DDI_HDEVICE hDevice
);

void APIENTRY PvgpuFlush(
    _In_ D3D10DDI_HDEVICE hDevice
);

/* ============================================================================
 * Resource Creation/Destruction
 * ============================================================================ */

SIZE_T APIENTRY PvgpuCalcPrivateResourceSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATERESOURCE* pCreateResource
);

void APIENTRY PvgpuCreateResource(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATERESOURCE* pCreateResource,
    _In_ D3D10DDI_HRESOURCE hResource,
    _In_ D3D10DDI_HRTRESOURCE hRTResource
);

void APIENTRY PvgpuDestroyResource(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hResource
);

void APIENTRY PvgpuOpenResource(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_OPENRESOURCE* pOpenResource,
    _In_ D3D10DDI_HRESOURCE hResource,
    _In_ D3D10DDI_HRTRESOURCE hRTResource
);

/* ============================================================================
 * Shader Creation/Destruction
 * ============================================================================ */

SIZE_T APIENTRY PvgpuCalcPrivateShaderSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures
);

void APIENTRY PvgpuCreateVertexShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ D3D10DDI_HRTSHADER hRTShader,
    _In_ CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures
);

void APIENTRY PvgpuCreatePixelShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ D3D10DDI_HRTSHADER hRTShader,
    _In_ CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures
);

void APIENTRY PvgpuCreateGeometryShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ D3D10DDI_HRTSHADER hRTShader,
    _In_ CONST D3D10DDIARG_STAGE_IO_SIGNATURES* pSignatures
);

void APIENTRY PvgpuCreateHullShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ D3D10DDI_HRTSHADER hRTShader,
    _In_ CONST D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES* pSignatures
);

void APIENTRY PvgpuCreateDomainShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST UINT* pCode,
    _In_ D3D10DDI_HSHADER hShader,
    _In_ D3D10DDI_HRTSHADER hRTShader,
    _In_ CONST D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES* pSignatures
);

void APIENTRY PvgpuDestroyShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader
);

/* ============================================================================
 * Pipeline State
 * ============================================================================ */

void APIENTRY PvgpuIaSetInputLayout(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HELEMENTLAYOUT hInputLayout
);

void APIENTRY PvgpuIaSetVertexBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers,
    _In_reads_(NumBuffers) CONST UINT* pStrides,
    _In_reads_(NumBuffers) CONST UINT* pOffsets
);

void APIENTRY PvgpuIaSetIndexBuffer(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hBuffer,
    _In_ DXGI_FORMAT Format,
    _In_ UINT Offset
);

void APIENTRY PvgpuIaSetTopology(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10_DDI_PRIMITIVE_TOPOLOGY PrimitiveTopology
);

void APIENTRY PvgpuVsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader
);

void APIENTRY PvgpuPsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader
);

void APIENTRY PvgpuGsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader
);

void APIENTRY PvgpuHsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader
);

void APIENTRY PvgpuDsSetShader(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADER hShader
);

void APIENTRY PvgpuSetRenderTargets(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_reads_(NumViews) CONST D3D10DDI_HRENDERTARGETVIEW* phRenderTargetView,
    _In_ UINT NumViews,
    _In_ UINT ClearSlots,
    _In_ D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView
);

void APIENTRY PvgpuSetViewports(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT NumViewports,
    _In_ UINT ClearViewports,
    _In_reads_(NumViewports) CONST D3D10_DDI_VIEWPORT* pViewports
);

void APIENTRY PvgpuSetScissorRects(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT NumRects,
    _In_ UINT ClearRects,
    _In_reads_(NumRects) CONST D3D10_DDI_RECT* pRects
);

void APIENTRY PvgpuSetBlendState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HBLENDSTATE hBlendState,
    _In_reads_(4) CONST FLOAT BlendFactor[4],
    _In_ UINT SampleMask
);

void APIENTRY PvgpuSetDepthStencilState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState,
    _In_ UINT StencilRef
);

void APIENTRY PvgpuSetRasterizerState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRASTERIZERSTATE hRasterizerState
);

/* ============================================================================
 * Draw Commands
 * ============================================================================ */

void APIENTRY PvgpuDraw(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT VertexCount,
    _In_ UINT StartVertexLocation
);

void APIENTRY PvgpuDrawIndexed(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT IndexCount,
    _In_ UINT StartIndexLocation,
    _In_ INT BaseVertexLocation
);

void APIENTRY PvgpuDrawInstanced(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT VertexCountPerInstance,
    _In_ UINT InstanceCount,
    _In_ UINT StartVertexLocation,
    _In_ UINT StartInstanceLocation
);

void APIENTRY PvgpuDrawIndexedInstanced(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT IndexCountPerInstance,
    _In_ UINT InstanceCount,
    _In_ UINT StartIndexLocation,
    _In_ INT BaseVertexLocation,
    _In_ UINT StartInstanceLocation
);

void APIENTRY PvgpuDrawAuto(
    _In_ D3D10DDI_HDEVICE hDevice
);

/* ============================================================================
 * Clear Commands
 * ============================================================================ */

void APIENTRY PvgpuClearRenderTargetView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRENDERTARGETVIEW hRenderTargetView,
    _In_reads_(4) CONST FLOAT ColorRGBA[4]
);

void APIENTRY PvgpuClearDepthStencilView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView,
    _In_ UINT ClearFlags,
    _In_ FLOAT Depth,
    _In_ UINT8 Stencil
);

/* ============================================================================
 * Resource Operations
 * ============================================================================ */

void APIENTRY PvgpuResourceCopy(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hDstResource,
    _In_ D3D10DDI_HRESOURCE hSrcResource
);

void APIENTRY PvgpuResourceCopyRegion(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hDstResource,
    _In_ UINT DstSubresource,
    _In_ UINT DstX,
    _In_ UINT DstY,
    _In_ UINT DstZ,
    _In_ D3D10DDI_HRESOURCE hSrcResource,
    _In_ UINT SrcSubresource,
    _In_opt_ CONST D3D10_DDI_BOX* pSrcBox
);

void APIENTRY PvgpuResourceUpdateSubresourceUP(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hDstResource,
    _In_ UINT DstSubresource,
    _In_opt_ CONST D3D10_DDI_BOX* pDstBox,
    _In_ CONST VOID* pSysMemUP,
    _In_ UINT RowPitch,
    _In_ UINT DepthPitch
);

void APIENTRY PvgpuResourceMap(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hResource,
    _In_ UINT Subresource,
    _In_ D3D10_DDI_MAP MapType,
    _In_ UINT MapFlags,
    _Out_ D3D10DDI_MAPPED_SUBRESOURCE* pMappedSubresource
);

void APIENTRY PvgpuResourceUnmap(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRESOURCE hResource,
    _In_ UINT Subresource
);

/* ============================================================================
 * Present
 * ============================================================================ */

void APIENTRY PvgpuPresent(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ DXGI_DDI_ARG_PRESENT* pPresentData
);

void APIENTRY PvgpuBlt(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ DXGI_DDI_ARG_BLT* pBltData
);

HRESULT APIENTRY PvgpuResizeBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ DXGI_DDI_ARG_RESIZEBUFFERS* pResizeData
);

/* ============================================================================
 * State Object Creation
 * ============================================================================ */

SIZE_T APIENTRY PvgpuCalcPrivateBlendStateSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_BLEND_DESC* pBlendDesc
);

void APIENTRY PvgpuCreateBlendState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_BLEND_DESC* pBlendDesc,
    _In_ D3D10DDI_HBLENDSTATE hBlendState,
    _In_ D3D10DDI_HRTBLENDSTATE hRTBlendState
);

void APIENTRY PvgpuDestroyBlendState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HBLENDSTATE hBlendState
);

SIZE_T APIENTRY PvgpuCalcPrivateRasterizerStateSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_RASTERIZER_DESC* pRasterizerDesc
);

void APIENTRY PvgpuCreateRasterizerState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_RASTERIZER_DESC* pRasterizerDesc,
    _In_ D3D10DDI_HRASTERIZERSTATE hRasterizerState,
    _In_ D3D10DDI_HRTRASTERIZERSTATE hRTRasterizerState
);

void APIENTRY PvgpuDestroyRasterizerState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRASTERIZERSTATE hRasterizerState
);

SIZE_T APIENTRY PvgpuCalcPrivateDepthStencilStateSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_DEPTH_STENCIL_DESC* pDepthStencilDesc
);

void APIENTRY PvgpuCreateDepthStencilState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_DEPTH_STENCIL_DESC* pDepthStencilDesc,
    _In_ D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState,
    _In_ D3D10DDI_HRTDEPTHSTENCILSTATE hRTDepthStencilState
);

void APIENTRY PvgpuDestroyDepthStencilState(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState
);

SIZE_T APIENTRY PvgpuCalcPrivateSamplerSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_SAMPLER_DESC* pSamplerDesc
);

void APIENTRY PvgpuCreateSampler(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10_DDI_SAMPLER_DESC* pSamplerDesc,
    _In_ D3D10DDI_HSAMPLER hSampler,
    _In_ D3D10DDI_HRTSAMPLER hRTSampler
);

void APIENTRY PvgpuDestroySampler(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSAMPLER hSampler
);

SIZE_T APIENTRY PvgpuCalcPrivateElementLayoutSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATEELEMENTLAYOUT* pCreateElementLayout
);

void APIENTRY PvgpuCreateElementLayout(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATEELEMENTLAYOUT* pCreateElementLayout,
    _In_ D3D10DDI_HELEMENTLAYOUT hElementLayout,
    _In_ D3D10DDI_HRTELEMENTLAYOUT hRTElementLayout
);

void APIENTRY PvgpuDestroyElementLayout(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HELEMENTLAYOUT hElementLayout
);

/* ============================================================================
 * View Creation
 * ============================================================================ */

SIZE_T APIENTRY PvgpuCalcPrivateRenderTargetViewSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATERENDERTARGETVIEW* pCreateRenderTargetView
);

void APIENTRY PvgpuCreateRenderTargetView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATERENDERTARGETVIEW* pCreateRenderTargetView,
    _In_ D3D10DDI_HRENDERTARGETVIEW hRenderTargetView,
    _In_ D3D10DDI_HRTRENDERTARGETVIEW hRTRenderTargetView
);

void APIENTRY PvgpuDestroyRenderTargetView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HRENDERTARGETVIEW hRenderTargetView
);

SIZE_T APIENTRY PvgpuCalcPrivateDepthStencilViewSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATEDEPTHSTENCILVIEW* pCreateDepthStencilView
);

void APIENTRY PvgpuCreateDepthStencilView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATEDEPTHSTENCILVIEW* pCreateDepthStencilView,
    _In_ D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView,
    _In_ D3D10DDI_HRTDEPTHSTENCILVIEW hRTDepthStencilView
);

void APIENTRY PvgpuDestroyDepthStencilView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView
);

SIZE_T APIENTRY PvgpuCalcPrivateShaderResourceViewSize(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATESHADERRESOURCEVIEW* pCreateShaderResourceView
);

void APIENTRY PvgpuCreateShaderResourceView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ CONST D3D10DDIARG_CREATESHADERRESOURCEVIEW* pCreateShaderResourceView,
    _In_ D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView,
    _In_ D3D10DDI_HRTSHADERRESOURCEVIEW hRTShaderResourceView
);

void APIENTRY PvgpuDestroyShaderResourceView(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView
);

/* ============================================================================
 * Constant Buffers and Shader Resources Binding
 * ============================================================================ */

void APIENTRY PvgpuVsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers
);

void APIENTRY PvgpuPsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers
);

void APIENTRY PvgpuGsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers
);

void APIENTRY PvgpuHsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers
);

void APIENTRY PvgpuDsSetConstantBuffers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT StartBuffer,
    _In_ UINT NumBuffers,
    _In_reads_(NumBuffers) CONST D3D10DDI_HRESOURCE* phBuffers
);

void APIENTRY PvgpuVsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews
);

void APIENTRY PvgpuPsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews
);

void APIENTRY PvgpuGsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews
);

void APIENTRY PvgpuHsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews
);

void APIENTRY PvgpuDsSetShaderResources(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumViews,
    _In_reads_(NumViews) CONST D3D10DDI_HSHADERRESOURCEVIEW* phShaderResourceViews
);

void APIENTRY PvgpuVsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers
);

void APIENTRY PvgpuPsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers
);

void APIENTRY PvgpuGsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers
);

void APIENTRY PvgpuHsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers
);

void APIENTRY PvgpuDsSetSamplers(
    _In_ D3D10DDI_HDEVICE hDevice,
    _In_ UINT Offset,
    _In_ UINT NumSamplers,
    _In_reads_(NumSamplers) CONST D3D10DDI_HSAMPLER* phSamplers
);

/* ============================================================================
 * Command Buffer Helpers
 * ============================================================================ */

/* Write a command to the device's command buffer */
BOOL PvgpuWriteCommand(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ UINT32 CommandType,
    _In_ const void* pPayload,
    _In_ SIZE_T PayloadSize
);

/* Flush command buffer to kernel mode */
void PvgpuFlushCommandBuffer(
    _In_ PVGPU_UMD_DEVICE* pDevice
);

/* Allocate a new host resource handle */
UINT32 PvgpuAllocateResourceHandle(
    _In_ PVGPU_UMD_DEVICE* pDevice
);

/* Get UMD resource from DDI handle */
PVGPU_UMD_RESOURCE* PvgpuGetResource(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ D3D10DDI_HRESOURCE hResource
);

/* ============================================================================
 * KMD Escape Helpers
 * ============================================================================ */

/* Call KMD escape interface */
HRESULT PvgpuEscape(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _Inout_ void* pEscapeData,
    _In_ SIZE_T EscapeDataSize
);

/* Initialize shared memory access via KMD escape */
HRESULT PvgpuInitSharedMemory(
    _In_ PVGPU_UMD_DEVICE* pDevice
);

/* Allocate from shared memory heap via KMD */
HRESULT PvgpuHeapAlloc(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ UINT32 Size,
    _In_ UINT32 Alignment,
    _Out_ UINT32* pOffset
);

/* Free shared memory heap allocation via KMD */
HRESULT PvgpuHeapFree(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ UINT32 Offset,
    _In_ UINT32 Size
);

/* Ring the doorbell to notify host of new commands */
HRESULT PvgpuRingDoorbell(
    _In_ PVGPU_UMD_DEVICE* pDevice
);

/* Wait for a fence value to complete */
HRESULT PvgpuWaitFence(
    _In_ PVGPU_UMD_DEVICE* pDevice,
    _In_ UINT64 FenceValue,
    _In_ UINT32 TimeoutMs
);

#ifdef __cplusplus
}
#endif

#endif /* PVGPU_UMD_H */
