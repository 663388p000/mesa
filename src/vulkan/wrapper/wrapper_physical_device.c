#include <math.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "wrapper_entrypoints.h"
#include "wrapper_trampolines.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common.h"
#include "util/os_misc.h"

static uint32_t
parse_vk_version_from_env()
{
   uint32_t apiVersion = 0, major = 0, minor = 0, patch = 0;

   const char *wrapper_vk_version = getenv("WRAPPER_VK_VERSION");

   if (wrapper_vk_version) {
      sscanf(wrapper_vk_version, "%d.%d.%d", &major, &minor, &patch);
      apiVersion = VK_MAKE_VERSION(major, minor, patch);
   }

   return apiVersion;
}

static VkResult
wrapper_setup_device_extensions(struct wrapper_physical_device *pdevice) {
   struct vk_device_extension_table *exts = &pdevice->vk.supported_extensions;
   VkExtensionProperties pdevice_extensions[VK_DEVICE_EXTENSION_COUNT];
   uint32_t pdevice_extension_count = VK_DEVICE_EXTENSION_COUNT;
   VkResult result;

   result = pdevice->dispatch_table.EnumerateDeviceExtensionProperties(
      pdevice->dispatch_handle, NULL, &pdevice_extension_count, pdevice_extensions);

   if (result != VK_SUCCESS)
      return result;

   *exts = wrapper_device_extensions;

   for (int i = 0; i < pdevice_extension_count; i++) {
      int idx;
      for (idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
         if (strcmp(vk_device_extensions[idx].extensionName,
                     pdevice_extensions[i].extensionName) == 0)
            break;
      }

      if (idx >= VK_DEVICE_EXTENSION_COUNT)
         continue;

      if (wrapper_filter_extensions.extensions[idx])
         continue;

      pdevice->base_supported_extensions.extensions[idx] =
         exts->extensions[idx] = true;
   }

   exts->KHR_present_wait = exts->KHR_timeline_semaphore;

   return VK_SUCCESS;
}

static void
wrapper_apply_device_extension_blacklist(struct wrapper_physical_device *physical_device) {
   char *blacklist = getenv("WRAPPER_EXTENSION_BLACKLIST");
   if (!blacklist)
      return;
   char *extension = strtok(blacklist, ",");
   while (extension != NULL) {
      for (int i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
         if (strstr(extension, vk_device_extensions[i].extensionName)) {
            WRAPPER_LOG(info, "Blacklisting extension %s", extension);
            physical_device->vk.supported_extensions.extensions[i] = false;
         }
      }
      extension = strtok(NULL, ",");
   }
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrapper_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdevice->instance, pName);
}

VkResult enumerate_physical_device(struct vk_instance *_instance)
{
   struct wrapper_instance *instance = (struct wrapper_instance *)_instance;
   VkPhysicalDevice physical_devices[16];
   uint32_t physical_device_count = 16;
   static int wrapper_disable_placed = -1;
   static int wrapper_dmaheap_cached = -1;
   static int wrapper_disable_present_wait = -1;
   VkResult result;

   result = instance->dispatch_table.EnumeratePhysicalDevices(
      instance->dispatch_handle, &physical_device_count, physical_devices);

   if (result != VK_SUCCESS)
      return result;

   for (int i = 0; i < physical_device_count; i++) {
      PFN_vkGetInstanceProcAddr get_instance_proc_addr;
      struct wrapper_physical_device *pdevice;

      pdevice = vk_zalloc(&_instance->alloc, sizeof(*pdevice), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      if (!pdevice)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      struct vk_physical_device_dispatch_table dispatch_table;
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wrapper_physical_device_entrypoints, true);
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wsi_physical_device_entrypoints, false);
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wrapper_physical_device_trampolines, false);

      result = vk_physical_device_init(&pdevice->vk,
                                       &instance->vk,
                                       NULL, NULL, NULL,
                                       &dispatch_table);
      if (result != VK_SUCCESS) {
         vk_free(&_instance->alloc, pdevice);
         return result;
      }

      pdevice->instance = instance;
      pdevice->dispatch_handle = physical_devices[i];
      get_instance_proc_addr = instance->dispatch_table.GetInstanceProcAddr;

      vk_physical_device_dispatch_table_load(&pdevice->dispatch_table,
                                             get_instance_proc_addr,
                                             instance->dispatch_handle);

	  if (wrapper_disable_placed == -1)
          wrapper_disable_placed = getenv("WRAPPER_DISABLE_PLACED") ? atoi(getenv("WRAPPER_DISABLE_PLACED")) : 0;

      if (wrapper_disable_present_wait == -1)
         wrapper_disable_present_wait = getenv("WRAPPER_DISABLE_PRESENT_WAIT") && atoi(getenv("WRAPPER_DISABLE_PRESENT_WAIT"));

      wrapper_setup_device_extensions(pdevice);
      wrapper_apply_device_extension_blacklist(pdevice);
      wrapper_setup_device_features(pdevice);

      struct vk_features *supported_features = &pdevice->vk.supported_features;
      pdevice->base_supported_features = *supported_features;
      supported_features->presentId = true;
      supported_features->multiViewport = true;
      supported_features->depthClamp = true;
      supported_features->depthBiasClamp = true;
      if (!wrapper_disable_placed) {
        pdevice->vk.supported_extensions.EXT_map_memory_placed = true;
        pdevice->vk.supported_extensions.KHR_map_memory2 = true;
      	supported_features->memoryMapPlaced = true;
      	supported_features->memoryUnmapReserve = true;
      } else {
        WRAPPER_LOG(info, "Disabling VK_EXT_map_memory_placed");
      	pdevice->vk.supported_extensions.EXT_map_memory_placed = false;
        pdevice->vk.supported_extensions.KHR_map_memory2 = false;
      	supported_features->memoryMapPlaced = false;
      	supported_features->memoryUnmapReserve = false;
      }
      supported_features->textureCompressionBC = true;
      supported_features->fillModeNonSolid = true;
      supported_features->shaderClipDistance = true;
      supported_features->shaderCullDistance = true;
      if (wrapper_disable_present_wait) {
         WRAPPER_LOG(info, "Disabling present wait");
         supported_features->presentWait = false;
         pdevice->vk.supported_extensions.KHR_present_wait = false;
      } else {
         supported_features->presentWait = supported_features->timelineSemaphore;
      }
      supported_features->swapchainMaintenance1 = true;
      supported_features->imageCompressionControlSwapchain = false;
      
      result = wsi_device_init(&pdevice->wsi_device,
                               wrapper_physical_device_to_handle(pdevice),
                               wrapper_wsi_proc_addr, &_instance->alloc, -1,
                               NULL, &(struct wsi_device_options){});
      if (result != VK_SUCCESS) {
         vk_physical_device_finish(&pdevice->vk);
         vk_free(&_instance->alloc, pdevice);
         return result;
      }
      pdevice->vk.wsi_device = &pdevice->wsi_device;
      pdevice->wsi_device.force_bgra8_unorm_first = true;

      pdevice->driver_properties = (VkPhysicalDeviceDriverProperties) {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
      };
      pdevice->properties2 = (VkPhysicalDeviceProperties2) {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
         .pNext = &pdevice->driver_properties,
      };
      pdevice->dispatch_table.GetPhysicalDeviceProperties2(
         pdevice->dispatch_handle, &pdevice->properties2);
         
      pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties(
         pdevice->dispatch_handle, &pdevice->memory_properties);
     
      const char *app_name = instance->vk.app_info.app_name
         ? instance->vk.app_info.app_name : "wrapper";

      const char *engine_name = instance->vk.app_info.engine_name
         ? instance->vk.app_info.engine_name : "wrapper";
      pdevice->wsi_device.engine_name = engine_name;

      const uint32_t engine_version = instance->vk.app_info.engine_version;

      /* HACK: Specific prop drivers workarounds for Adreno and Mali GPUs */
      
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
         if (strstr(engine_name, "DXVK")) {
            WRAPPER_LOG(info, "Disabling VK_EXT_line_rasterization");
            pdevice->vk.supported_extensions.EXT_line_rasterization = false;	
            if (engine_version >= VK_MAKE_VERSION(2, 7, 0)) {
               WRAPPER_LOG(info, "Faking VK_KHR_pipeline_library");
               pdevice->vk.supported_extensions.KHR_pipeline_library = true;
            }
         }
         if (pdevice->properties2.properties.driverVersion > VK_MAKE_VERSION(512, 744, 0) &&
             strstr(app_name, "clvk")) {
            WRAPPER_LOG(info, "Disabling globalPriorityQueue feature");
            supported_features->globalPriorityQuery = false;    
         }
      }

      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_ARM_PROPRIETARY) {
         if (strstr(engine_name, "DXVK")) {
            WRAPPER_LOG(info, "Faking VK_EXT_robustness2");
            pdevice->vk.supported_extensions.EXT_robustness2 = true;
            WRAPPER_LOG(info, "Disabling VK_EXT_extended_dynamic_state and VK_EXT_extended_dynamic_state2");
            pdevice->vk.supported_extensions.EXT_extended_dynamic_state = false;
            pdevice->vk.supported_extensions.EXT_extended_dynamic_state2 = false;
         }
         WRAPPER_LOG(info, "Disabling VK_EXT_calibrated_timestamps");
         pdevice->vk.supported_extensions.EXT_calibrated_timestamps = false;
      }

      if (wrapper_dmaheap_cached == -1)
         wrapper_dmaheap_cached = getenv("WRAPPER_DMAHEAP_CACHED") && atoi(getenv("WRAPPER_DMAHEAP_CACHED"));

      if (wrapper_dmaheap_cached)
         pdevice->dma_heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
      else
         pdevice->dma_heap_fd = open("/dev/dma_heap/system-uncached", O_RDONLY | O_CLOEXEC);
         
      if (pdevice->dma_heap_fd < 0)
         pdevice->dma_heap_fd = open("/dev/ion", O_RDONLY);

      char *wrapper_resource_type = getenv("WRAPPER_RESOURCE_TYPE");

      pdevice->resource_type = wrapper_resource_type ? wrapper_resource_type : "auto";

      list_addtail(&pdevice->vk.link, &_instance->physical_devices.list);
   }

   return VK_SUCCESS;
}

void destroy_physical_device(struct vk_physical_device *pdevice) {
   VK_FROM_HANDLE(wrapper_physical_device, wpdevice,
                  vk_physical_device_to_handle(pdevice));
   if (wpdevice->dma_heap_fd != -1)
      close(wpdevice->dma_heap_fd);
   wsi_device_finish(pdevice->wsi_device, &pdevice->instance->alloc);
   vk_physical_device_finish(pdevice);
   vk_free(&pdevice->instance->alloc, pdevice);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                           const char* pLayerName,
                                           uint32_t* pPropertyCount,
                                           VkExtensionProperties* pProperties)
{
   return vk_common_EnumerateDeviceExtensionProperties(physicalDevice,
                                                       pLayerName,
                                                       pPropertyCount,
                                                       pProperties);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceFeatures* pFeatures) 
{
   return vk_common_GetPhysicalDeviceFeatures(physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                   VkPhysicalDeviceFeatures2* pFeatures) {                                                              
   vk_common_GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                    VkPhysicalDeviceProperties *pProperties)
{
   char *device_name;
   uint32_t api_version = parse_vk_version_from_env();
   
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceProperties(
      pdevice->dispatch_handle, pProperties);

   asprintf(&device_name, "Wrapper(%s)", pProperties->deviceName);
   strcpy(pProperties->deviceName, device_name);

   if (api_version > 0)
      pProperties->apiVersion = api_version;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                     VkPhysicalDeviceProperties2* pProperties)
{
   char *device_name;
   char *driver_info;
   uint32_t api_version = parse_vk_version_from_env();
   
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceProperties2(
      pdevice->dispatch_handle, pProperties);

   asprintf(&device_name, "Wrapper(%s)", pProperties->properties.deviceName);
   strcpy(pProperties->properties.deviceName, device_name);

   if (api_version > 0)
      pProperties->properties.apiVersion = api_version;

   vk_foreach_struct(prop, pProperties->pNext) {
      switch (prop->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT:
      {
         VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *placed_prop =
               (VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *)prop;
         uint64_t os_page_size;
         os_get_page_size(&os_page_size);
         placed_prop->minPlacedMemoryMapAlignment = os_page_size;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES_EXT:
      {
      	 VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT *texel_prop =
      	      (VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT *)prop;
      	 texel_prop->storageTexelBufferOffsetAlignmentBytes = 1;
      	 texel_prop->uniformTexelBufferOffsetAlignmentBytes = 1;
      	 break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR:
      {
         VkPhysicalDeviceFloatControlsPropertiesKHR *float_prop =
              (VkPhysicalDeviceFloatControlsPropertiesKHR *)prop;
         float_prop->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
         float_prop->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;     
         float_prop->shaderDenormFlushToZeroFloat16 = false;
         float_prop->shaderDenormFlushToZeroFloat32 = false;
         float_prop->shaderRoundingModeRTEFloat16 = false;
         float_prop->shaderRoundingModeRTEFloat32 = false;
         float_prop->shaderSignedZeroInfNanPreserveFloat16 = false;
         float_prop->shaderSignedZeroInfNanPreserveFloat32 = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
      {
         VkPhysicalDeviceVulkan11Properties *vk11_prop =
              (VkPhysicalDeviceVulkan11Properties *)prop;
         vk11_prop->subgroupSupportedOperations = 0;
         vk11_prop->subgroupSupportedStages = 0;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
      {
         VkPhysicalDeviceVulkan12Properties *vk12_prop =
              (VkPhysicalDeviceVulkan12Properties *)prop;
         vk12_prop->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
         vk12_prop->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
         vk12_prop->shaderDenormFlushToZeroFloat16 = false;
         vk12_prop->shaderDenormFlushToZeroFloat32 = false;
         vk12_prop->shaderRoundingModeRTEFloat16 = false;
         vk12_prop->shaderRoundingModeRTEFloat32 = false;
         vk12_prop->shaderSignedZeroInfNanPreserveFloat16 = false;
         vk12_prop->shaderSignedZeroInfNanPreserveFloat32 = false;
 
         asprintf(&driver_info, "%d.%d.%d", 
            VK_VERSION_MAJOR(pProperties->properties.driverVersion),
            VK_VERSION_MINOR(pProperties->properties.driverVersion),
            VK_VERSION_PATCH(pProperties->properties.driverVersion));
         
         strcpy(vk12_prop->driverInfo, driver_info);
         strcpy(vk12_prop->driverName, "Wrapper driver");
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
      {
         VkPhysicalDeviceVulkan13Properties *vk13_prop =
              (VkPhysicalDeviceVulkan13Properties *)prop;
         vk13_prop->storageTexelBufferOffsetAlignmentBytes = 1;
         vk13_prop->uniformTexelBufferOffsetAlignmentBytes = 1;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:
      {
         VkPhysicalDeviceSubgroupProperties *subgroup_prop =
              (VkPhysicalDeviceSubgroupProperties *)prop;
         subgroup_prop->supportedOperations = 0;
         subgroup_prop->supportedStages = 0;
         break;
      }
      default:
         break;
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice,
	                                           VkFormat format,
	                                           VkImageType type,
	                                           VkImageTiling tiling,
	                                           VkImageUsageFlags usage,
	                                           VkImageCreateFlags flags,
	                                           VkImageFormatProperties *pImageFormatProperties)
{
   VkResult result;
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   result = pdevice->dispatch_table.GetPhysicalDeviceImageFormatProperties(
      pdevice->dispatch_handle, format, type, tiling, usage, flags, pImageFormatProperties);
      
   switch(format) {
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:                                    
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC2_UNORM_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK:
   case VK_FORMAT_BC3_UNORM_BLOCK:
   case VK_FORMAT_BC3_SRGB_BLOCK:
   case VK_FORMAT_BC4_UNORM_BLOCK:
   case VK_FORMAT_BC4_SNORM_BLOCK:
   case VK_FORMAT_BC5_UNORM_BLOCK:
   case VK_FORMAT_BC5_SNORM_BLOCK:
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
   case VK_FORMAT_BC6H_SFLOAT_BLOCK:
   case VK_FORMAT_BC7_UNORM_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      if (type & VK_IMAGE_TYPE_1D) {
         pImageFormatProperties->maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension1D;
         pImageFormatProperties->maxExtent.height = 1;
         pImageFormatProperties->maxExtent.depth = 1;
      }
      if (type & VK_IMAGE_TYPE_2D) {
         if (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
            pImageFormatProperties->maxExtent.width = pdevice->properties2.properties.limits.maxImageDimensionCube;
            pImageFormatProperties->maxExtent.height = pdevice->properties2.properties.limits.maxImageDimensionCube;
         }
         else {
            pImageFormatProperties->maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension2D;
            pImageFormatProperties->maxExtent.height = pdevice->properties2.properties.limits.maxImageDimension2D;
         }
         pImageFormatProperties->maxExtent.depth = 1;
      }
      if (type & VK_IMAGE_TYPE_3D) {
         pImageFormatProperties->maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension3D;
         pImageFormatProperties->maxExtent.height = pdevice->properties2.properties.limits.maxImageDimension3D;
         pImageFormatProperties->maxExtent.depth = pdevice->properties2.properties.limits.maxImageDimension3D;
      }
      if (tiling & VK_IMAGE_TILING_LINEAR ||
             tiling & VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ||
             flags & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT)
             pImageFormatProperties->maxMipLevels = 1;
      else 
         pImageFormatProperties->maxMipLevels = log2(
            pImageFormatProperties->maxExtent.width > pImageFormatProperties->maxExtent.height ? pImageFormatProperties->maxExtent.width :  pImageFormatProperties->maxExtent.height 	
         );
    
      if (tiling & VK_IMAGE_TILING_LINEAR ||
            ((tiling & VK_IMAGE_TILING_OPTIMAL) && type & VK_IMAGE_TYPE_3D))
         pImageFormatProperties->maxArrayLayers = 1;
      else
         pImageFormatProperties->maxArrayLayers = pdevice->properties2.properties.limits.maxImageArrayLayers;
      // We do not handle any case here for now
      pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;      
      pImageFormatProperties->maxResourceSize = 562949953421312;
      return VK_SUCCESS;
   default:
      break;
   }

   return result;   
}	                                           

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
                                                const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
                                                VkImageFormatProperties2* pImageFormatProperties)
{
   VkResult result;
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   
   result = pdevice->dispatch_table.GetPhysicalDeviceImageFormatProperties2(
      pdevice->dispatch_handle, pImageFormatInfo, pImageFormatProperties);

   switch(pImageFormatInfo->format) {
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:                                    
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC2_UNORM_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK:
   case VK_FORMAT_BC3_UNORM_BLOCK:
   case VK_FORMAT_BC3_SRGB_BLOCK:
   case VK_FORMAT_BC4_UNORM_BLOCK:
   case VK_FORMAT_BC4_SNORM_BLOCK:
   case VK_FORMAT_BC5_UNORM_BLOCK:
   case VK_FORMAT_BC5_SNORM_BLOCK:
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
   case VK_FORMAT_BC6H_SFLOAT_BLOCK:
   case VK_FORMAT_BC7_UNORM_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      if (pImageFormatInfo->type & VK_IMAGE_TYPE_1D) {
         pImageFormatProperties->imageFormatProperties.maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension1D;
         pImageFormatProperties->imageFormatProperties.maxExtent.height = 1;
         pImageFormatProperties->imageFormatProperties.maxExtent.depth = 1;
      }
      if (pImageFormatInfo->type & VK_IMAGE_TYPE_2D) {
         if (pImageFormatInfo->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
            pImageFormatProperties->imageFormatProperties.maxExtent.width = pdevice->properties2.properties.limits.maxImageDimensionCube;
            pImageFormatProperties->imageFormatProperties.maxExtent.height = pdevice->properties2.properties.limits.maxImageDimensionCube;
         }
         else {
            pImageFormatProperties->imageFormatProperties.maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension2D;
            pImageFormatProperties->imageFormatProperties.maxExtent.height = pdevice->properties2.properties.limits.maxImageDimension2D;
         }
         pImageFormatProperties->imageFormatProperties.maxExtent.depth = 1;
      }
      if (pImageFormatInfo->type & VK_IMAGE_TYPE_3D) {
         pImageFormatProperties->imageFormatProperties.maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension3D;
         pImageFormatProperties->imageFormatProperties.maxExtent.height = pdevice->properties2.properties.limits.maxImageDimension3D;
         pImageFormatProperties->imageFormatProperties.maxExtent.depth = pdevice->properties2.properties.limits.maxImageDimension3D;
      }
      // We do not handle the case where vkPhysicalDeviceImageFormatInfo pNext has
      // a handleType which does not require mipMaps
      if (pImageFormatInfo->tiling & VK_IMAGE_TILING_LINEAR ||
             pImageFormatInfo->tiling & VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ||
             pImageFormatInfo->flags & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT)
             pImageFormatProperties->imageFormatProperties.maxMipLevels = 1;
      else 
         pImageFormatProperties->imageFormatProperties.maxMipLevels = log2(
            pImageFormatProperties->imageFormatProperties.maxExtent.width > pImageFormatProperties->imageFormatProperties.maxExtent.height ? pImageFormatProperties->imageFormatProperties.maxExtent.width :  pImageFormatProperties->imageFormatProperties.maxExtent.height 	
         );
    
      if (pImageFormatInfo->tiling & VK_IMAGE_TILING_LINEAR ||
            ((pImageFormatInfo->tiling & VK_IMAGE_TILING_OPTIMAL) && pImageFormatInfo->type & VK_IMAGE_TYPE_3D))
         pImageFormatProperties->imageFormatProperties.maxArrayLayers = 1;
      else
         pImageFormatProperties->imageFormatProperties.maxArrayLayers = pdevice->properties2.properties.limits.maxImageArrayLayers;
      // We do not handle any case here for now
      pImageFormatProperties->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;      
      pImageFormatProperties->imageFormatProperties.maxResourceSize = 562949953421312;
      return VK_SUCCESS;
   default:
      break;
   }

   return result;
}                                                

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                            VkFormat format,
                                            VkFormatProperties* pFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceFormatProperties(
      pdevice->dispatch_handle, format, pFormatProperties);
      
   switch (format) {
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC2_UNORM_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK:
   case VK_FORMAT_BC3_UNORM_BLOCK:
   case VK_FORMAT_BC3_SRGB_BLOCK:
   case VK_FORMAT_BC4_UNORM_BLOCK:
   case VK_FORMAT_BC4_SNORM_BLOCK:
   case VK_FORMAT_BC5_UNORM_BLOCK:
   case VK_FORMAT_BC5_SNORM_BLOCK:
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
   case VK_FORMAT_BC6H_SFLOAT_BLOCK:
   case VK_FORMAT_BC7_UNORM_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      pFormatProperties->optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      break;
   default:
      break;   
   }
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
										  VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   static int wrapper_vmem_max_size = -1;
   
   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties(
      pdevice->dispatch_handle, pMemoryProperties);

   if (wrapper_vmem_max_size == -1)
      wrapper_vmem_max_size = getenv("WRAPPER_VMEM_MAX_SIZE") ? atoi(getenv("WRAPPER_VMEM_MAX_SIZE")) : -1;

   if (wrapper_vmem_max_size > 0)
      pMemoryProperties->memoryHeaps[0].size = (VkDeviceSize)wrapper_vmem_max_size * 1048576;   
}

VKAPI_ATTR void VKAPI_CALL                                                                                      
wrapper_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                           VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   static int wrapper_vmem_max_size = -1;

   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties2(
      pdevice->dispatch_handle, pMemoryProperties);

   if (wrapper_vmem_max_size == -1)
      wrapper_vmem_max_size = getenv("WRAPPER_VMEM_MAX_SIZE") ? atoi(getenv("WRAPPER_VMEM_MAX_SIZE")) : -1;

   if (wrapper_vmem_max_size > 0)
      pMemoryProperties->memoryProperties.memoryHeaps[0].size = (VkDeviceSize)wrapper_vmem_max_size * 1048576;
}								
