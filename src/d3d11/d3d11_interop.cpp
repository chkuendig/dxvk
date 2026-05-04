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


  HRESULT STDMETHODCALLTYPE D3D11VkInterop::CopySurfaceToExternalBuffer(
          IDXGIVkInteropSurface*  pSrc,
          VkBuffer                Dst,
          UINT64                  DstSize,
          UINT                    DstRowPitch) {
    /* TODO: submit vkCmdCopyImageToBuffer on the DXVK queue and wait via
     * a CPU-side fence. For the first iteration of this interface the
     * caller (ZLUDA) gets uninitialised memory in the buffer; the
     * register/import path can still be validated end-to-end without a
     * working blit. PhysX will read zeros. */
    Logger::warn("D3D11VkInterop::CopySurfaceToExternalBuffer: blit not yet implemented");
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