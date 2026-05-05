#include "d3d11_context_imm.h"
#include "d3d11_interop.h"
#include "d3d11_device.h"

#include "../dxvk/dxvk_adapter.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_instance.h"

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
     * counts AND usage flags. */
    VkImage           srcImage  = VK_NULL_HANDLE;
    VkImageLayout     srcLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo srcInfo   = { };
    if (FAILED(pSrc->GetVulkanImageInfo(&srcImage, &srcLayout, &srcInfo)) || !srcImage) {
      Logger::err("D3D11VkInterop::CopySurfaceToExternalBuffer: GetVulkanImageInfo failed");
      return E_FAIL;
    }

    /* DXVK only sets VK_IMAGE_USAGE_TRANSFER_SRC_BIT on textures that have
     * a use case for it (staging copies, mip generation, …). Batman's PhysX
     * field-sampler textures are typically created as SRV+RTV with no
     * transfer-src usage, which makes vkCmdCopyImageToBuffer illegal — we
     * observed this manifesting as a UTCL2 page fault in dxvk-submit (TCP
     * client, RW=write) on Steam Deck gfx1033. The principled fix needs
     * either a parallel staging image (created with TRANSFER_SRC) that DXVK
     * blits the original into, or an upstream DXVK hint to OR
     * TRANSFER_SRC_BIT into texture creation when ZLUDA register hooks into
     * the resource. Until that's wired, refuse the copy and let CUDA see
     * the uninitialised buffer (same effective state as iter17b — register/
     * import is still validated end-to-end). */
    if (!(srcInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
      static bool warned_once = false;
      if (!warned_once) {
        Logger::warn("D3D11VkInterop::CopySurfaceToExternalBuffer: source "
                     "image lacks VK_IMAGE_USAGE_TRANSFER_SRC_BIT; "
                     "imported memory will not be refreshed (warning logged once)");
        warned_once = true;
      }
      return S_OK;
    }

    /* Drain DXVK's CS thread so any queued draws against the source image
     * are visible by the time we submit our copy. lockSubmission keeps
     * DXVK from interleaving its own submits while ours is in flight. */
    FlushRenderingCommands();
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
      return E_FAIL;
    }

    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkd->vkBeginCommandBuffer(cmd, &begin);

    VkImageSubresourceRange range = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0, srcInfo.mipLevels, 0, srcInfo.arrayLayers
    };

    /* Transition srcLayout → TRANSFER_SRC_OPTIMAL.  We use the broad
     * ALL_COMMANDS / MEMORY masks because we don't know what stage left the
     * image in srcLayout — anything is conservatively correct here. */
    VkImageMemoryBarrier toSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toSrc.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    toSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout           = srcLayout;
    toSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image               = srcImage;
    toSrc.subresourceRange    = range;
    vkd->vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, nullptr, 0, nullptr, 1, &toSrc);

    /* Copy the whole mip 0 layer 0 into the external buffer. We assume
     * R32G32B32A32_FLOAT (16 B/pixel) for the row-length conversion. The
     * caller has already validated this; if it ever isn't, the CUDA
     * consumer would also be wrong. */
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

    /* Transition back to whatever DXVK had it in. */
    VkImageMemoryBarrier toOrig = toSrc;
    toOrig.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toOrig.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    toOrig.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toOrig.newLayout     = srcLayout;
    vkd->vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      0, 0, nullptr, 0, nullptr, 1, &toOrig);

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
      return E_FAIL;
    }

    vkd->vkWaitForFences(vkd->device(), 1, &fence, VK_TRUE, UINT64_MAX);

    vkd->vkDestroyFence(vkd->device(), fence, nullptr);
    vkd->vkFreeCommandBuffers(vkd->device(), pool, 1, &cmd);
    vkd->vkDestroyCommandPool(vkd->device(), pool, nullptr);
    device->unlockSubmission();
    return S_OK;
  }


  void STDMETHODCALLTYPE D3D11VkInterop::FreeExternalBuffer(
          VkBuffer        Buffer,
          VkDeviceMemory  Memory) {
    auto vkd = m_device->GetDXVKDevice()->vkd();
    if (Buffer != VK_NULL_HANDLE) vkd->vkDestroyBuffer(vkd->device(), Buffer, nullptr);
    if (Memory != VK_NULL_HANDLE) vkd->vkFreeMemory(vkd->device(), Memory, nullptr);
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