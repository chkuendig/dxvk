#include "d3d11_context_imm.h"
#include "d3d11_interop.h"
#include "d3d11_device.h"

#include "../dxvk/dxvk_adapter.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_instance.h"

#include <atomic>

namespace dxvk {
  
  D3D11VkInterop::D3D11VkInterop(
          IDXGIObject*          pContainer,
          D3D11Device*          pDevice)
  : m_container (pContainer),
    m_device    (pDevice) { }
  
  
  D3D11VkInterop::~D3D11VkInterop() {
    
  }
  
  
  ULONG STDMETHODCALLTYPE D3D11VkInterop::AddRef() {
    return m_container->AddRef();
  }
  
  
  ULONG STDMETHODCALLTYPE D3D11VkInterop::Release() {
    return m_container->Release();
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11VkInterop::QueryInterface(
          REFIID                riid,
          void**                ppvObject) {
    return m_container->QueryInterface(riid, ppvObject);
  }
  
  
  void STDMETHODCALLTYPE D3D11VkInterop::GetVulkanHandles(
          VkInstance*           pInstance,
          VkPhysicalDevice*     pPhysDev,
          VkDevice*             pDevice) {
    auto device   = m_device->GetDXVKDevice();
    auto adapter  = device->adapter();
    auto instance = device->instance();
    
    if (pDevice != nullptr)
      *pDevice = device->handle();
    
    if (pPhysDev != nullptr)
      *pPhysDev = adapter->handle();
    
    if (pInstance != nullptr)
      *pInstance = instance->handle();
  }
  
  
  void STDMETHODCALLTYPE D3D11VkInterop::GetSubmissionQueue(
          VkQueue*              pQueue,
          uint32_t*             pQueueFamilyIndex) {
    auto device = static_cast<D3D11Device*>(m_device)->GetDXVKDevice();
    DxvkDeviceQueue queue = device->queues().graphics;
    
    if (pQueue != nullptr)
      *pQueue = queue.queueHandle;
    
    if (pQueueFamilyIndex != nullptr)
      *pQueueFamilyIndex = queue.queueFamily;
  }
  
  
  void STDMETHODCALLTYPE D3D11VkInterop::TransitionSurfaceLayout(
          IDXGIVkInteropSurface*    pSurface,
    const VkImageSubresourceRange*  pSubresources,
          VkImageLayout             OldLayout,
          VkImageLayout             NewLayout) {
    auto immediateContext = m_device->GetContext();

    immediateContext->TransitionSurfaceLayout(
      pSurface, pSubresources, OldLayout, NewLayout);
  }
  
  
  void STDMETHODCALLTYPE D3D11VkInterop::FlushRenderingCommands() {
    auto immediateContext = m_device->GetContext();
    immediateContext->Flush();
    immediateContext->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
  }
  
  
  void STDMETHODCALLTYPE D3D11VkInterop::LockSubmissionQueue() {
    m_device->GetDXVKDevice()->lockSubmission();
  }
  
  
  void STDMETHODCALLTYPE D3D11VkInterop::ReleaseSubmissionQueue() {
    m_device->GetDXVKDevice()->unlockSubmission();
  }
  
  
  void STDMETHODCALLTYPE D3D11VkInterop::GetSubmissionQueue1(
          VkQueue*              pQueue,
          uint32_t*             pQueueIndex,
          uint32_t*             pQueueFamilyIndex) {
    auto device = static_cast<D3D11Device*>(m_device)->GetDXVKDevice();
    DxvkDeviceQueue queue = device->queues().graphics;
    
    if (pQueue != nullptr)
      *pQueue = queue.queueHandle;
    
    if (pQueueIndex != nullptr)
      *pQueueIndex = queue.queueIndex;
    
    if (pQueueFamilyIndex != nullptr)
      *pQueueFamilyIndex = queue.queueFamily;
  }

  /* ZLUDA D3D11 interop extensions — allocate an external buffer with
   * VK_KHR_external_memory_win32 export bits and hand the HANDLE plus
   * underlying VkBuffer / VkDeviceMemory back to the caller. Used by
   * nvidia-libs/.../wine_cuGraphicsD3D11RegisterResource because the
   * relevant KHR functions can't be resolved from PE-side application
   * code in Wine — they only exist inside DXVK's per-device function
   * table. See chkuendig/dxvk@zluda-physx for fork details. */
  HRESULT STDMETHODCALLTYPE D3D11VkInterop::AllocateExternalBuffer(
          UINT64                Size,
          HANDLE*               pHandle,
          VkBuffer*             pBufferOut,
          VkDeviceMemory*       pMemoryOut,
          UINT64*               pAllocSizeOut) {
    if (!pHandle || !pBufferOut || !pMemoryOut)
      return E_POINTER;

    auto device  = m_device->GetDXVKDevice();
    auto vkd     = device->vkd();
    auto adapter = device->adapter();

    /* Buffer with external memory bits */
    VkExternalMemoryBufferCreateInfo extBufInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    extBufInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkBufferCreateInfo bufInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &extBufInfo };
    bufInfo.size        = Size;
    bufInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                        | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                        | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buf = VK_NULL_HANDLE;
    if (vkd->vkCreateBuffer(vkd->device(), &bufInfo, nullptr, &buf) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::AllocateExternalBuffer: vkCreateBuffer failed");
      return E_FAIL;
    }

    VkMemoryRequirements req = { };
    vkd->vkGetBufferMemoryRequirements(vkd->device(), buf, &req);

    /* Find a DEVICE_LOCAL memory type that satisfies typeBits */
    auto memProps = adapter->memoryProperties();
    int32_t memTypeIdx = -1;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
      if (!(req.memoryTypeBits & (1u << i))) continue;
      if (!(memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
      memTypeIdx = int32_t(i);
      break;
    }
    if (memTypeIdx < 0) {
      Logger::err("D3D11VkInterop::AllocateExternalBuffer: no DEVICE_LOCAL memory type");
      vkd->vkDestroyBuffer(vkd->device(), buf, nullptr);
      return E_FAIL;
    }

    VkExportMemoryAllocateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo };
    allocInfo.allocationSize  = req.size;
    allocInfo.memoryTypeIndex = uint32_t(memTypeIdx);

    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkd->vkAllocateMemory(vkd->device(), &allocInfo, nullptr, &mem) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::AllocateExternalBuffer: vkAllocateMemory failed");
      vkd->vkDestroyBuffer(vkd->device(), buf, nullptr);
      return E_FAIL;
    }

    if (vkd->vkBindBufferMemory(vkd->device(), buf, mem, 0) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::AllocateExternalBuffer: vkBindBufferMemory failed");
      vkd->vkFreeMemory(vkd->device(), mem, nullptr);
      vkd->vkDestroyBuffer(vkd->device(), buf, nullptr);
      return E_FAIL;
    }

    /* Export the memory as an NT HANDLE (Wine wraps the underlying dma_buf
     * fd). The KHR function lives in the DXVK device function table and is
     * resolved correctly because DXVK's vkCreateDevice enabled the
     * VK_KHR_external_memory_win32 extension (see
     * chkuendig/dxvk@zluda-physx dxvk_extensions.h). */
#ifdef VK_KHR_external_memory_win32
    VkMemoryGetWin32HandleInfoKHR handleInfo = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
    handleInfo.memory     = mem;
    handleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    HANDLE handle = nullptr;
    if (vkd->vkGetMemoryWin32HandleKHR(vkd->device(), &handleInfo, &handle) != VK_SUCCESS || !handle) {
      Logger::err("D3D11VkInterop::AllocateExternalBuffer: vkGetMemoryWin32HandleKHR failed");
      vkd->vkFreeMemory(vkd->device(), mem, nullptr);
      vkd->vkDestroyBuffer(vkd->device(), buf, nullptr);
      return E_FAIL;
    }
#else
    HANDLE handle = nullptr;
    Logger::err("D3D11VkInterop::AllocateExternalBuffer: VK_KHR_external_memory_win32 not compiled in");
    vkd->vkFreeMemory(vkd->device(), mem, nullptr);
    vkd->vkDestroyBuffer(vkd->device(), buf, nullptr);
    return E_FAIL;
#endif

    *pHandle      = handle;
    *pBufferOut   = buf;
    *pMemoryOut   = mem;
    if (pAllocSizeOut) *pAllocSizeOut = req.size;
    return S_OK;
  }


  /* Blit a DXVK image into our external buffer. The data flow is one-way:
   * DXVK (current image contents) → external buffer → CUDA reads via
   * hipExternalMemoryGetMappedBuffer. Synchronisation is CPU-side because
   * hipImportExternalSemaphore is stubbed on Linux ROCm; this introduces
   * a per-Map stall on the order of a few hundred microseconds, which
   * matches PhysX' Map-then-launch cadence well enough.
   *
   * Note that we currently assume R32G32B32A32_FLOAT (16 B/pixel) and a
   * single mip / single array layer. The PE-side caller validates the
   * descriptor before reaching here, so anything else would already have
   * fallen back to the public cuGraphicsD3D11RegisterResource path. */
  HRESULT STDMETHODCALLTYPE D3D11VkInterop::CopySurfaceToExternalBuffer(
          IDXGIVkInteropSurface*  pSrc,
          VkBuffer                Dst,
          UINT64                  DstSize,
          UINT                    DstRowPitch) {
    if (!pSrc || Dst == VK_NULL_HANDLE)
      return E_POINTER;

    auto device = m_device->GetDXVKDevice();
    auto vkd    = device->vkd();

    /* GetVulkanImageInfo gives us the VkImage, the layout DXVK left it in,
     * and the original VkImageCreateInfo so we know extent + mip/array
     * counts AND usage flags.
     *
     * D3D11VkInteropSurface::GetVulkanImageInfo (d3d11_texture.cpp) rejects
     * the call with E_INVALIDARG unless pInfo->sType is set to
     * VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO and pNext is null — easy to miss
     * because no other Vulkan struct that we hand TO Vulkan needs sType
     * pre-set when DXVK is the one filling it in. Without this line the
     * function returns failure, the blit never runs, and CUDA reads
     * uninitialised memory (silent data corruption — burned a session on
     * mis-diagnosing this as a TRANSFER_SRC_BIT issue). */
    VkImage           srcImage  = VK_NULL_HANDLE;
    VkImageLayout     srcLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo srcInfo   = { };
    srcInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    if (FAILED(pSrc->GetVulkanImageInfo(&srcImage, &srcLayout, &srcInfo)) || !srcImage) {
      Logger::err("D3D11VkInterop::CopySurfaceToExternalBuffer: GetVulkanImageInfo failed");
      return E_FAIL;
    }

    /* Defensive: every D3D11Texture in DXVK is created with TRANSFER_SRC_BIT
     * (d3d11_texture.cpp:38) so this branch should never fire. Keep the
     * check + one-time warning so we get a loud signal if a future DXVK
     * change drops the bit conditionally. */
    if (!(srcInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
      static bool warned_once = false;
      if (!warned_once) {
        Logger::warn("D3D11VkInterop::CopySurfaceToExternalBuffer: source "
                     "image lacks VK_IMAGE_USAGE_TRANSFER_SRC_BIT — "
                     "DXVK contract changed?");
        warned_once = true;
      }
      return E_FAIL;
    }

    /* Use DXVK's TransitionSurfaceLayout to let DXVK's CS-thread bookkeeping
     * track the layout change. Bypassing it (with a raw vkCmdPipelineBarrier
     * on a private command buffer) caused dxvk-submit page faults roughly
     * half the time on gfx1033 — RADV disables DCC compression metadata on
     * the transition out of COLOR_ATTACHMENT_OPTIMAL, and DXVK's next use
     * of the image read decompressed bytes through DCC-expecting paths.
     * TransitionSurfaceLayout queues to the CS thread; FlushRenderingCommands
     * drains and submits, so by the time we lockSubmission the image is in
     * TRANSFER_SRC_OPTIMAL and DXVK knows. */
    VkImageSubresourceRange range = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0, srcInfo.mipLevels, 0, srcInfo.arrayLayers
    };

    TransitionSurfaceLayout(pSrc, &range, srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    FlushRenderingCommands();

    /* lockSubmission keeps DXVK from interleaving its own submits while ours
     * is in flight. CS thread is already drained by FlushRenderingCommands. */
    device->lockSubmission();

    /* Per-call command pool / fence — Batman maps in batches of 6–12 a few
     * times per second, so the create/destroy overhead is small relative
     * to the GPU stall on vkWaitForFences. Could be cached if profiling
     * shows it matters. */
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.queueFamilyIndex = device->queues().graphics.queueFamily;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkd->vkCreateCommandPool(vkd->device(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::CopySurfaceToExternalBuffer: vkCreateCommandPool failed");
      device->unlockSubmission();
      TransitionSurfaceLayout(pSrc, &range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout);
      FlushRenderingCommands();
      return E_FAIL;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmdAlloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdAlloc.commandPool        = pool;
    cmdAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    if (vkd->vkAllocateCommandBuffers(vkd->device(), &cmdAlloc, &cmd) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::CopySurfaceToExternalBuffer: vkAllocateCommandBuffers failed");
      vkd->vkDestroyCommandPool(vkd->device(), pool, nullptr);
      device->unlockSubmission();
      TransitionSurfaceLayout(pSrc, &range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout);
      FlushRenderingCommands();
      return E_FAIL;
    }

    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkd->vkBeginCommandBuffer(cmd, &begin);

    /* Image is already in TRANSFER_SRC_OPTIMAL via DXVK's transition above —
     * just record the copy itself. We assume R32G32B32A32_FLOAT (16 B/pixel)
     * for the row-length conversion; PE-side caller validates this format. */
    constexpr uint32_t bytesPerPixel = 16; /* R32G32B32A32_FLOAT */
    VkBufferImageCopy region = { };
    region.bufferOffset      = 0;
    region.bufferRowLength   = DstRowPitch ? (DstRowPitch / bytesPerPixel) : 0;
    region.bufferImageHeight = 0;
    region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset       = { 0, 0, 0 };
    region.imageExtent       = srcInfo.extent;
    vkd->vkCmdCopyImageToBuffer(cmd, srcImage,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Dst, 1, &region);

    vkd->vkEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkd->vkCreateFence(vkd->device(), &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::CopySurfaceToExternalBuffer: vkCreateFence failed");
      vkd->vkFreeCommandBuffers(vkd->device(), pool, 1, &cmd);
      vkd->vkDestroyCommandPool(vkd->device(), pool, nullptr);
      device->unlockSubmission();
      return E_FAIL;
    }

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    auto queue = device->queues().graphics.queueHandle;
    if (vkd->vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::CopySurfaceToExternalBuffer: vkQueueSubmit failed");
      vkd->vkDestroyFence(vkd->device(), fence, nullptr);
      vkd->vkFreeCommandBuffers(vkd->device(), pool, 1, &cmd);
      vkd->vkDestroyCommandPool(vkd->device(), pool, nullptr);
      device->unlockSubmission();
      TransitionSurfaceLayout(pSrc, &range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout);
      FlushRenderingCommands();
      return E_FAIL;
    }

    vkd->vkWaitForFences(vkd->device(), 1, &fence, VK_TRUE, UINT64_MAX);

    vkd->vkDestroyFence(vkd->device(), fence, nullptr);
    vkd->vkFreeCommandBuffers(vkd->device(), pool, 1, &cmd);
    vkd->vkDestroyCommandPool(vkd->device(), pool, nullptr);
    device->unlockSubmission();

    /* Hand the image back to DXVK in its original layout. */
    TransitionSurfaceLayout(pSrc, &range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout);
    FlushRenderingCommands();

    /* Positive trace: log the FIRST successful blit so we can verify in the
     * dxvk log that the path actually executed end-to-end (not just
     * returned S_OK silently). One-shot to avoid spam. */
    static std::atomic<bool> first_blit_logged { false };
    bool expected = false;
    if (first_blit_logged.compare_exchange_strong(expected, true)) {
      Logger::warn(str::format("D3D11VkInterop::CopySurfaceToExternalBuffer: "
                               "first blit submitted+waited OK ",
                               srcInfo.extent.width, "x", srcInfo.extent.height,
                               " fmt=", srcInfo.format,
                               " pitch=", DstRowPitch, " size=", DstSize));
    }
    return S_OK;
  }


  void STDMETHODCALLTYPE D3D11VkInterop::FreeExternalBuffer(
          VkBuffer        Buffer,
          VkDeviceMemory  Memory) {
    auto vkd = m_device->GetDXVKDevice()->vkd();
    if (Buffer != VK_NULL_HANDLE) vkd->vkDestroyBuffer(vkd->device(), Buffer, nullptr);
    if (Memory != VK_NULL_HANDLE) vkd->vkFreeMemory(vkd->device(), Memory, nullptr);
  }


  /* Reverse blit — propagate CUDA-written buffer contents back into the
   * DXVK image so D3D11's next sample sees them. Mirror of
   * CopySurfaceToExternalBuffer with vkCmdCopyBufferToImage and inverted
   * access masks. Same per-call submission cost; same CPU fence wait. */
  HRESULT STDMETHODCALLTYPE D3D11VkInterop::CopyExternalBufferToSurface(
          VkBuffer                Src,
          UINT64                  SrcSize,
          UINT                    SrcRowPitch,
          IDXGIVkInteropSurface*  pDst) {
    if (!pDst || Src == VK_NULL_HANDLE)
      return E_POINTER;

    auto device = m_device->GetDXVKDevice();
    auto vkd    = device->vkd();

    VkImage           dstImage  = VK_NULL_HANDLE;
    VkImageLayout     dstLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo dstInfo   = { };
    dstInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    if (FAILED(pDst->GetVulkanImageInfo(&dstImage, &dstLayout, &dstInfo)) || !dstImage) {
      Logger::err("D3D11VkInterop::CopyExternalBufferToSurface: GetVulkanImageInfo failed");
      return E_FAIL;
    }
    if (!(dstInfo.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      static bool warned_once = false;
      if (!warned_once) {
        Logger::warn("D3D11VkInterop::CopyExternalBufferToSurface: dst image "
                     "lacks VK_IMAGE_USAGE_TRANSFER_DST_BIT — DXVK contract changed?");
        warned_once = true;
      }
      return E_FAIL;
    }

    /* Same DXVK-cooperative pattern as CopySurfaceToExternalBuffer:
     * route layout transitions through DXVK's CS thread so its bookkeeping
     * stays consistent (DCC compression metadata, shader-readable state,
     * etc). Bypassing this with raw vkCmdPipelineBarrier on a private
     * cmdbuf was the source of the dxvk-submit page faults observed
     * pre-refactor. */
    VkImageSubresourceRange range = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0, dstInfo.mipLevels, 0, dstInfo.arrayLayers
    };

    TransitionSurfaceLayout(pDst, &range, dstLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    FlushRenderingCommands();

    device->lockSubmission();

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.queueFamilyIndex = device->queues().graphics.queueFamily;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkd->vkCreateCommandPool(vkd->device(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::CopyExternalBufferToSurface: vkCreateCommandPool failed");
      device->unlockSubmission();
      TransitionSurfaceLayout(pDst, &range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstLayout);
      FlushRenderingCommands();
      return E_FAIL;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmdAlloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdAlloc.commandPool        = pool;
    cmdAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    if (vkd->vkAllocateCommandBuffers(vkd->device(), &cmdAlloc, &cmd) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::CopyExternalBufferToSurface: vkAllocateCommandBuffers failed");
      vkd->vkDestroyCommandPool(vkd->device(), pool, nullptr);
      device->unlockSubmission();
      TransitionSurfaceLayout(pDst, &range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstLayout);
      FlushRenderingCommands();
      return E_FAIL;
    }

    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkd->vkBeginCommandBuffer(cmd, &begin);

    /* Image is already in TRANSFER_DST_OPTIMAL via DXVK's transition above. */
    constexpr uint32_t bytesPerPixel = 16; /* R32G32B32A32_FLOAT */
    VkBufferImageCopy region = { };
    region.bufferOffset      = 0;
    region.bufferRowLength   = SrcRowPitch ? (SrcRowPitch / bytesPerPixel) : 0;
    region.bufferImageHeight = 0;
    region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset       = { 0, 0, 0 };
    region.imageExtent       = dstInfo.extent;
    vkd->vkCmdCopyBufferToImage(cmd, Src, dstImage,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    vkd->vkEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkd->vkCreateFence(vkd->device(), &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::CopyExternalBufferToSurface: vkCreateFence failed");
      vkd->vkFreeCommandBuffers(vkd->device(), pool, 1, &cmd);
      vkd->vkDestroyCommandPool(vkd->device(), pool, nullptr);
      device->unlockSubmission();
      return E_FAIL;
    }

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    auto queue = device->queues().graphics.queueHandle;
    if (vkd->vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) {
      Logger::err("D3D11VkInterop::CopyExternalBufferToSurface: vkQueueSubmit failed");
      vkd->vkDestroyFence(vkd->device(), fence, nullptr);
      vkd->vkFreeCommandBuffers(vkd->device(), pool, 1, &cmd);
      vkd->vkDestroyCommandPool(vkd->device(), pool, nullptr);
      device->unlockSubmission();
      TransitionSurfaceLayout(pDst, &range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstLayout);
      FlushRenderingCommands();
      return E_FAIL;
    }

    vkd->vkWaitForFences(vkd->device(), 1, &fence, VK_TRUE, UINT64_MAX);

    vkd->vkDestroyFence(vkd->device(), fence, nullptr);
    vkd->vkFreeCommandBuffers(vkd->device(), pool, 1, &cmd);
    vkd->vkDestroyCommandPool(vkd->device(), pool, nullptr);
    device->unlockSubmission();

    /* Hand the image back to DXVK in its original layout. */
    TransitionSurfaceLayout(pDst, &range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstLayout);
    FlushRenderingCommands();

    static std::atomic<bool> first_reverse_logged { false };
    bool expected = false;
    if (first_reverse_logged.compare_exchange_strong(expected, true)) {
      Logger::warn(str::format("D3D11VkInterop::CopyExternalBufferToSurface: "
                               "first reverse blit submitted+waited OK ",
                               dstInfo.extent.width, "x", dstInfo.extent.height,
                               " fmt=", dstInfo.format,
                               " pitch=", SrcRowPitch, " size=", SrcSize));
    }
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11VkInterop::CreateTexture2DFromVkImage(
          const D3D11_TEXTURE2D_DESC1 *pDesc,
          VkImage vkImage,
          ID3D11Texture2D **ppTexture2D) {

    InitReturnPtr(ppTexture2D);

    if (!pDesc)
      return E_INVALIDARG;
    
    D3D11_COMMON_TEXTURE_DESC desc;
    desc.Width          = pDesc->Width;
    desc.Height         = pDesc->Height;
    desc.Depth          = 1;
    desc.MipLevels      = pDesc->MipLevels;
    desc.ArraySize      = pDesc->ArraySize;
    desc.Format         = pDesc->Format;
    desc.SampleDesc     = pDesc->SampleDesc;
    desc.Usage          = pDesc->Usage;
    desc.BindFlags      = pDesc->BindFlags;
    desc.CPUAccessFlags = pDesc->CPUAccessFlags;
    desc.MiscFlags      = pDesc->MiscFlags;
    desc.TextureLayout  = pDesc->TextureLayout;
    
    HRESULT hr = D3D11CommonTexture::NormalizeTextureProperties(&desc);

    if (FAILED(hr))
      return hr;
    
    if (!ppTexture2D)
      return S_FALSE;
    
    try {
      Com<D3D11Texture2D> texture = new D3D11Texture2D(m_device, &desc, 0, vkImage);
      *ppTexture2D = texture.ref();
      return S_OK;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return E_INVALIDARG;
    }
  }
}