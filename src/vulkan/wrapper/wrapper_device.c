#include <sys/stat.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "wrapper_bcdec.h"
#include "spirv_patcher.hpp"
#include "wrapper_entrypoints.h"
#include "wrapper_trampolines.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "vk_queue.h"
#include "vk_util.h"
#include "util/list.h"
#include "util/simple_mtx.h"

#define WRAPPER_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

/*
 * ---------------------------------------------------------------------
 * Extension policy
 * ---------------------------------------------------------------------
 *
 * wrapper_device_extensions    - extensions the wrapper itself owns,
 *                                 emulates, or otherwise always wants
 *                                 advertised. Never re-added by the
 *                                 "enable everything" pass below.
 *
 * wrapper_mandatory_extensions - extensions device creation *requires*
 *                                 unconditionally from the underlying
 *                                 driver (currently just
 *                                 buffer_device_address). Checked and
 *                                 force-enabled explicitly in
 *                                 wrapper_CreateDevice(); skipped by
 *                                 the "enable everything" pass so they
 *                                 are never added twice.
 *
 * wrapper_filter_extensions    - extensions the wrapper deliberately
 *                                 withholds from the driver.
 *
 * wrapper_known_bugs           - a small, explicit table of extensions
 *                                 withheld only on specific driver IDs
 *                                 because of a known, real bug there.
 *
 * Separately, wrapper_mali_valhall_table + the wrapper_mali_* helpers
 * below implement a *conditionally* mandatory set (dynamic rendering +
 * multiview) that only applies to Mali Valhall GPUs with more than one
 * shader core (MC2/MP2 and above) -- see wrapper_mali_requires_dynamic_rendering().
 * ---------------------------------------------------------------------
 */

const struct vk_device_extension_table wrapper_device_extensions =
{
   .KHR_swapchain = true,
   .EXT_swapchain_maintenance1 = true,
   .KHR_swapchain_mutable_format = true,
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   .EXT_display_control = true,
#endif
   .KHR_present_id = true,
   .KHR_present_wait = true,
   .KHR_incremental_present = true,
   .KHR_driver_properties = true,
   .KHR_device_group = true,
   .KHR_image_format_list = true,
   .KHR_zero_initialize_workgroup_memory = true,
   .EXT_multisampled_render_to_single_sampled = true,
};

const struct vk_device_extension_table wrapper_mandatory_extensions =
{
   .KHR_buffer_device_address = true,
   /*
    * Skipped here so wrapper_enable_all_driver_extensions() never
    * double-adds them; actually requested/enforced only when
    * wrapper_mali_requires_dynamic_rendering() is true (see below).
    */
   .KHR_dynamic_rendering = true,
   .KHR_dynamic_rendering_local_read = true,
   .EXT_dynamic_rendering_unused_attachments = true,
   .KHR_multiview = true,
};

const struct vk_device_extension_table wrapper_filter_extensions =
{
   .EXT_hdr_metadata = true,
   .GOOGLE_display_timing = true,
   .KHR_shared_presentable_image = true,
   .EXT_image_compression_control_swapchain = true,
};

struct wrapper_known_bug_entry {
   VkDriverId driver_id;
   const char *extension_name;
   const char *note;
};

/*
 * Keep this table short and evidence-based. Each entry should map to
 * a genuinely observed, driver-specific bug -- not a hunch. Gate the
 * whole mechanism behind WRAPPER_IGNORE_KNOWN_BUGS=1 for bisecting.
 */
static const struct wrapper_known_bug_entry wrapper_known_bugs[] = {
   {
      .driver_id = VK_DRIVER_ID_ARM_PROPRIETARY,
      .extension_name = "VK_EXT_transform_feedback",
      .note = "Unstable transform feedback + robustness2 interaction on some Mali driver builds",
   },
   {
      .driver_id = VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
      .extension_name = "VK_EXT_multisampled_render_to_single_sampled",
      .note = "Resolve-target corruption observed on some legacy Adreno driver builds",
   },
};

static bool
wrapper_extension_has_known_bug(struct wrapper_physical_device *pdevice,
                                const char *extension_name)
{
   static int wrapper_ignore_known_bugs = -1;

   if (wrapper_ignore_known_bugs == -1) {
      wrapper_ignore_known_bugs = getenv("WRAPPER_IGNORE_KNOWN_BUGS") ?
         atoi(getenv("WRAPPER_IGNORE_KNOWN_BUGS")) : 0;
   }

   if (wrapper_ignore_known_bugs)
      return false;

   VkDriverId driver_id = pdevice->driver_properties.driverID;

   for (size_t i = 0; i < WRAPPER_ARRAY_LEN(wrapper_known_bugs); i++) {
      if (wrapper_known_bugs[i].driver_id != driver_id)
         continue;
      if (strcmp(wrapper_known_bugs[i].extension_name, extension_name) != 0)
         continue;

      WRAPPER_LOG(info, "Withholding %s: %s", extension_name,
                  wrapper_known_bugs[i].note);
      return true;
   }

   return false;
}

/*
 * ---------------------------------------------------------------------
 * Mali Valhall GPU / driver identification
 * ---------------------------------------------------------------------
 *
 * GPU id is *never* assumed -- it is always read back from the device
 * via VkPhysicalDeviceProperties.deviceID (which on the ARM proprietary
 * driver carries the raw GPU_ID register value; the upper 16 bits are
 * the product id used below), gated on vendorID/driverID actually being
 * ARM/Mali first. Only once that identity is confirmed do we consult
 * the table and decide what to enforce.
 * ---------------------------------------------------------------------
 */

#define WRAPPER_MALI_VENDOR_ID 0x13B5

struct wrapper_mali_gpu_entry {
   const char *name;
   const char *architecture;
   uint16_t product_id;    /* upper 16 bits of the raw GPU_ID register */
   uint32_t model_id_raw;  /* full raw GPU_ID register value */
};

static const struct wrapper_mali_gpu_entry wrapper_mali_valhall_table[] = {
   { "Mali-G57",   "Valhall v9",  0x9001, 0x90010000 },
   { "Mali-G57",   "Valhall v9",  0x9003, 0x90030000 },
   { "Mali-G68",   "Valhall v9",  0x9004, 0x90040000 },
   { "Mali-G77",   "Valhall v9",  0x9000, 0x90000000 },
   { "Mali-G78",   "Valhall v9",  0x9002, 0x90020000 },
   { "Mali-G78AE", "Valhall v9",  0x9005, 0x90050000 },
   { "Mali-G310",  "Valhall v10", 0xA004, 0xA0040000 },
   { "Mali-G510",  "Valhall v10", 0xA003, 0xA0030000 },
   { "Mali-G610",  "Valhall v10", 0xA007, 0xA0070000 },
   { "Mali-G710",  "Valhall v10", 0xA002, 0xA0020000 },
   { "Mali-G615",  "Valhall v11", 0xB003, 0xB0030000 },
   { "Mali-G715",  "Valhall v11", 0xB002, 0xB0020000 },
};

/* Step 1: confirm this is actually an ARM Mali device, then look the
 * driver-reported GPU id up in the Valhall table. Returns NULL for
 * anything that isn't ARM/Mali or isn't a recognized Valhall part. */
static const struct wrapper_mali_gpu_entry *
wrapper_lookup_mali_gpu(struct wrapper_physical_device *pdevice)
{
   if (pdevice->properties2.properties.vendorID != WRAPPER_MALI_VENDOR_ID)
      return NULL;

   if (pdevice->driver_properties.driverID != VK_DRIVER_ID_ARM_PROPRIETARY)
      return NULL;

   uint32_t gpu_id = pdevice->properties2.properties.deviceID;
   uint16_t product_id = (uint16_t)(gpu_id >> 16);

   for (size_t i = 0; i < WRAPPER_ARRAY_LEN(wrapper_mali_valhall_table); i++) {
      if (wrapper_mali_valhall_table[i].product_id == product_id)
         return &wrapper_mali_valhall_table[i];
   }

   return NULL;
}

/* Step 2: once we know it's a Valhall part, read the shader-core count
 * off the driver-reported name string ("Mali-G710 MC10", or the older
 * "MPx" naming). Never guessed -- if the driver doesn't report a count
 * we treat it as unknown and do not enforce anything. */
static int
wrapper_mali_core_count(struct wrapper_physical_device *pdevice)
{
   const char *name = pdevice->properties2.properties.deviceName;

   const char *suffix = strstr(name, "MC");
   if (!suffix)
      suffix = strstr(name, "MP");
   if (!suffix)
      return -1;

   int count = atoi(suffix + 2);
   return count > 0 ? count : -1;
}

/* Step 3: identify, then decide. Only Valhall GPUs reporting MC2/MP2
 * or higher require dynamic rendering + multiview; MC1/MP1 and unknown
 * core counts are exempt. */
static bool
wrapper_mali_requires_dynamic_rendering(struct wrapper_physical_device *pdevice)
{
   const struct wrapper_mali_gpu_entry *gpu = wrapper_lookup_mali_gpu(pdevice);
   if (!gpu)
      return false;

   int core_count = wrapper_mali_core_count(pdevice);
   if (core_count < 2)
      return false;

   WRAPPER_LOG(info,
      "Detected %s (%s, GPU_ID 0x%08x, %d shader cores) -- enforcing "
      "VK_KHR_dynamic_rendering / VK_KHR_dynamic_rendering_local_read / "
      "VK_EXT_dynamic_rendering_unused_attachments / VK_KHR_multiview",
      gpu->name, gpu->architecture,
      pdevice->properties2.properties.deviceID, core_count);

   return true;
}

/* Hard gate: device creation must not proceed if a Valhall MC2+ part
 * is detected but the driver can't actually back the requirement. */
static VkResult
wrapper_check_valhall_mandatory_extensions(struct wrapper_physical_device *pdevice)
{
   bool have_dynamic_rendering =
      pdevice->base_supported_extensions.KHR_dynamic_rendering ||
      pdevice->properties2.properties.apiVersion >= VK_API_VERSION_1_3;
   bool have_local_read =
      pdevice->base_supported_extensions.KHR_dynamic_rendering_local_read;
   bool have_unused_attachments =
      pdevice->base_supported_extensions.EXT_dynamic_rendering_unused_attachments;
   bool have_multiview =
      pdevice->base_supported_extensions.KHR_multiview ||
      pdevice->properties2.properties.apiVersion >= VK_API_VERSION_1_1;

   if (!have_dynamic_rendering || !have_local_read ||
       !have_unused_attachments || !have_multiview) {
      WRAPPER_LOG(error,
         "Mali Valhall MC2+ GPU detected but the driver is missing a "
         "mandatory extension (dynamic_rendering=%d local_read=%d "
         "unused_attachments=%d multiview=%d)",
         have_dynamic_rendering, have_local_read,
         have_unused_attachments, have_multiview);
      return VK_ERROR_EXTENSION_NOT_PRESENT;
   }

   if (!pdevice->base_supported_features.dynamicRendering ||
       !pdevice->base_supported_features.dynamicRenderingLocalRead ||
       !pdevice->base_supported_features.dynamicRenderingUnusedAttachments ||
       !pdevice->base_supported_features.multiview) {
      WRAPPER_LOG(error,
         "Mali Valhall MC2+ GPU detected but the driver does not expose "
         "the matching feature bits");
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   return VK_SUCCESS;
}

static void
wrapper_append_valhall_extensions(uint32_t *count, const char **exts)
{
   exts[(*count)++] = "VK_KHR_dynamic_rendering";
   exts[(*count)++] = "VK_KHR_dynamic_rendering_local_read";
   exts[(*count)++] = "VK_EXT_dynamic_rendering_unused_attachments";
   exts[(*count)++] = "VK_KHR_multiview";
}

/*
 * Force the four Valhall-mandatory feature bits on, the same
 * "mutate in place if present, splice a struct in if not" pattern as
 * wrapper_force_buffer_device_address() below. dynamicRendering and
 * multiview are also recognized via their core-promoted aggregate
 * structs (Vulkan 1.3 / 1.1 features) to avoid adding a redundant,
 * spec-invalid duplicate struct when the app already requested the
 * core version struct.
 */
static void
wrapper_force_valhall_features(VkBaseInStructure *create_info,
   VkPhysicalDeviceDynamicRenderingFeatures *dr_storage,
   VkPhysicalDeviceDynamicRenderingLocalReadFeatures *drlr_storage,
   VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT *dru_storage,
   VkPhysicalDeviceMultiviewFeatures *mv_storage)
{
   bool have_dynamic_rendering = false;
   bool have_local_read = false;
   bool have_unused_attachments = false;
   bool have_multiview = false;

   for (VkBaseInStructure *current = (VkBaseInStructure *)create_info->pNext;
        current != NULL; current = (VkBaseInStructure *)current->pNext) {
      switch (current->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
         ((VkPhysicalDeviceVulkan13Features *)current)->dynamicRendering = VK_TRUE;
         have_dynamic_rendering = true;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
         ((VkPhysicalDeviceVulkan11Features *)current)->multiview = VK_TRUE;
         have_multiview = true;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
         ((VkPhysicalDeviceDynamicRenderingFeatures *)current)->dynamicRendering = VK_TRUE;
         have_dynamic_rendering = true;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES:
         ((VkPhysicalDeviceDynamicRenderingLocalReadFeatures *)current)->dynamicRenderingLocalRead = VK_TRUE;
         have_local_read = true;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT:
         ((VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT *)current)->dynamicRenderingUnusedAttachments = VK_TRUE;
         have_unused_attachments = true;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
         ((VkPhysicalDeviceMultiviewFeatures *)current)->multiview = VK_TRUE;
         have_multiview = true;
         break;
      default:
         break;
      }
   }

   if (!have_dynamic_rendering) {
      dr_storage->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
      dr_storage->dynamicRendering = VK_TRUE;
      dr_storage->pNext = create_info->pNext;
      create_info->pNext = (VkBaseInStructure *)dr_storage;
   }
   if (!have_local_read) {
      drlr_storage->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES;
      drlr_storage->dynamicRenderingLocalRead = VK_TRUE;
      drlr_storage->pNext = create_info->pNext;
      create_info->pNext = (VkBaseInStructure *)drlr_storage;
   }
   if (!have_unused_attachments) {
      dru_storage->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT;
      dru_storage->dynamicRenderingUnusedAttachments = VK_TRUE;
      dru_storage->pNext = create_info->pNext;
      create_info->pNext = (VkBaseInStructure *)dru_storage;
   }
   if (!have_multiview) {
      mv_storage->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
      mv_storage->multiview = VK_TRUE;
      mv_storage->pNext = create_info->pNext;
      create_info->pNext = (VkBaseInStructure *)mv_storage;
   }
}

static struct wrapper_buffer *
get_wrapper_buffer_from_handle(struct wrapper_device *device, VkBuffer buffer) {
   struct wrapper_buffer *wb = NULL;

   simple_mtx_lock(&device->resource_mutex);
   wb = _mesa_hash_table_u64_search(device->buffer_table, (uint64_t) buffer);
   simple_mtx_unlock(&device->resource_mutex);

   return wb;
}

static struct wrapper_image *
get_wrapper_image_from_handle(struct wrapper_device *device, VkImage image) {
   struct wrapper_image *wi = NULL;
   
   simple_mtx_lock(&device->resource_mutex);
   wi = _mesa_hash_table_u64_search(device->image_table, (uint64_t) image);
   simple_mtx_unlock(&device->resource_mutex);
   
   return wi;
}

static struct wrapper_fence *
get_wrapper_fence_from_handle(struct wrapper_device *device, VkFence fence) {
   struct wrapper_fence *wf = NULL;

   simple_mtx_lock(&device->resource_mutex);
   wf = _mesa_hash_table_u64_search(device->fence_table, (uint64_t) fence);
   simple_mtx_unlock(&device->resource_mutex);

   return wf;
}

/*
 * Enable every extension the driver supports, beyond what the app
 * explicitly asked for. This makes the wrapper transparently expose
 * the full capability of the underlying driver rather than only the
 * subset an app happened to request.
 *
 * Extensions already owned by the wrapper (wrapper_device_extensions),
 * already mandatory (wrapper_mandatory_extensions -- handled via their
 * own dedicated append/force paths instead), deliberately withheld
 * (wrapper_filter_extensions), or flagged with a known driver bug are
 * skipped here so each extension is only ever added to the enable
 * list from a single place.
 */
static void
wrapper_enable_all_driver_extensions(struct wrapper_device *device,
                                     uint32_t *enable_extension_count,
                                     const char **enable_extensions)
{
   struct wrapper_physical_device *pdevice = device->physical;

   for (int idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
      if (!pdevice->base_supported_extensions.extensions[idx])
         continue;

      if (wrapper_device_extensions.extensions[idx])
         continue;

      if (wrapper_mandatory_extensions.extensions[idx])
         continue;

      if (wrapper_filter_extensions.extensions[idx])
         continue;

      const char *extension_name = vk_device_extensions[idx].extensionName;

      if (wrapper_extension_has_known_bug(pdevice, extension_name))
         continue;

      enable_extensions[(*enable_extension_count)++] = extension_name;
   }
}

static inline void
wrapper_append_required_extensions(const struct vk_device *device,
                                  uint32_t *count,
                                  const char **exts) {
#define REQUIRED_EXTENSION(name) \
   if (device->physical->supported_extensions.name) { \
      exts[(*count)++] = "VK_" #name; \
   }
   
   REQUIRED_EXTENSION(KHR_external_fence);
   REQUIRED_EXTENSION(KHR_external_semaphore);
   REQUIRED_EXTENSION(KHR_external_memory);
   REQUIRED_EXTENSION(KHR_external_fence_fd);
   REQUIRED_EXTENSION(KHR_external_semaphore_fd);
   REQUIRED_EXTENSION(KHR_external_memory_fd);
   REQUIRED_EXTENSION(KHR_dedicated_allocation);
   REQUIRED_EXTENSION(EXT_queue_family_foreign);
   REQUIRED_EXTENSION(KHR_maintenance1)
   REQUIRED_EXTENSION(KHR_maintenance2)
   REQUIRED_EXTENSION(KHR_image_format_list)
   REQUIRED_EXTENSION(KHR_swapchain);
   REQUIRED_EXTENSION(KHR_timeline_semaphore);
   REQUIRED_EXTENSION(EXT_external_memory_host);
   REQUIRED_EXTENSION(EXT_external_memory_dma_buf);
   REQUIRED_EXTENSION(EXT_image_drm_format_modifier);
   REQUIRED_EXTENSION(ANDROID_external_memory_android_hardware_buffer);
   /* Mandatory for this wrapper -- checked/force-enabled explicitly in
    * wrapper_CreateDevice(); listed here too so the extension *string*
    * enable path stays consistent with every other required extension. */
   REQUIRED_EXTENSION(KHR_buffer_device_address);
   REQUIRED_EXTENSION(KHR_driver_properties);
   REQUIRED_EXTENSION(KHR_device_group);
   REQUIRED_EXTENSION(KHR_zero_initialize_workgroup_memory);
   REQUIRED_EXTENSION(EXT_multisampled_render_to_single_sampled);
#undef REQUIRED_EXTENSION
}

static void unlink_vk_struct(VkBaseInStructure *create_info, const VkBaseInStructure **current, VkBaseInStructure **prev) {
   if (!*prev) 
      create_info->pNext = (*current)->pNext;
   else
      (*prev)->pNext = (*current)->pNext;                                                

   *current = (*current)->pNext;
}

/*
 * Force VK_KHR_buffer_device_address / core bufferDeviceAddress on.
 * The wrapper treats this feature as mandatory and actually used
 * internally, so it can't just be advertised -- it must be enabled
 * regardless of whether (or how) the app asked for it.
 *
 * If the app's pNext chain already carries a struct with the bit,
 * flip it in place (same "mutate the app's request" pattern already
 * used by the DISABLE_FEATURE macro in wrapper_CreateDevice). If not,
 * splice `storage` into the (already-copied) wrapper create-info
 * chain. `storage` must outlive the driver's vkCreateDevice() call.
 */
static void
wrapper_force_buffer_device_address(VkBaseInStructure *create_info,
                                    VkPhysicalDeviceBufferDeviceAddressFeatures *storage)
{
   VkBaseInStructure *current = (VkBaseInStructure *)create_info->pNext;

   while (current != NULL) {
      if (current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
         ((VkPhysicalDeviceBufferDeviceAddressFeatures *)current)->bufferDeviceAddress = VK_TRUE;
         return;
      }
      if (current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
         ((VkPhysicalDeviceVulkan12Features *)current)->bufferDeviceAddress = VK_TRUE;
         return;
      }
      current = (VkBaseInStructure *)current->pNext;
   }

   storage->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
   storage->bufferDeviceAddress = VK_TRUE;
   storage->pNext = create_info->pNext;
   create_info->pNext = (VkBaseInStructure *)storage;
}

static void process_pnext_chain(VkBaseInStructure *create_info, struct wrapper_physical_device *pdevice) {
   const uint32_t api_version = pdevice->properties2.properties.apiVersion;
   const VkBaseInStructure *current = (VkBaseInStructure *)create_info->pNext;
   VkBaseInStructure *prev = NULL;

   while (current != NULL) {
      switch(current->sType) {
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
             VkPhysicalDeviceTransformFeedbackFeaturesEXT *transform_features =
                (VkPhysicalDeviceTransformFeedbackFeaturesEXT *)current;
             transform_features->geometryStreams &= pdevice->base_supported_features.geometryStreams;
             break;
          }
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
             if (pdevice->base_supported_extensions.EXT_robustness2)
                break;
             WRAPPER_LOG(info, "Unlinking VkPhysicalDeviceRobustness2FeaturesEXT from pNext chain");
             unlink_vk_struct(create_info, &current, &prev);
             continue;
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
             if (api_version >= VK_MAKE_VERSION(1, 1, 0))
                break;
             WRAPPER_LOG(info, "Unlinking VkPhysicalDeviceVulkan11Features from pNext chain");
             unlink_vk_struct(create_info, &current, &prev);
             continue;
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
             if (api_version >= VK_MAKE_VERSION(1, 2, 0))
                break;
             WRAPPER_LOG(info, "Unlinking VkPhysicalDeviceVulkan12Features from pNext chain");
             unlink_vk_struct(create_info, &current, &prev);
             continue;
          case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
             if (api_version < VK_MAKE_VERSION(1, 3, 0)) {
                WRAPPER_LOG(info, "Unlinking VkPhysicalDeviceVulkan13Features from pNext chain");
                unlink_vk_struct(create_info, &current, &prev);
                continue;
             }

             /*
              * Driver reports Vulkan 1.3 support. Rather than either
              * trusting the whole struct blindly or stripping it
              * wholesale like the 1.1/1.2 fallback path above, clamp
              * every individual bit to what this specific driver
              * actually advertises -- "optimize for 1.3, but stay
              * dependent on the driver".
              */
             VkPhysicalDeviceVulkan13Features *features13 =
                (VkPhysicalDeviceVulkan13Features *)current;
#define CLAMP13(f) features13->f &= pdevice->base_supported_features.f
             CLAMP13(robustImageAccess);
             CLAMP13(inlineUniformBlock);
             CLAMP13(descriptorBindingInlineUniformBlockUpdateAfterBind);
             CLAMP13(pipelineCreationCacheControl);
             CLAMP13(privateData);
             CLAMP13(shaderDemoteToHelperInvocation);
             CLAMP13(shaderTerminateInvocation);
             CLAMP13(subgroupSizeControl);
             CLAMP13(computeFullSubgroups);
             CLAMP13(synchronization2);
             CLAMP13(textureCompressionASTC_HDR);
             CLAMP13(shaderZeroInitializeWorkgroupMemory);
             CLAMP13(dynamicRendering);
             CLAMP13(shaderIntegerDotProduct);
             CLAMP13(maintenance4);
#undef CLAMP13
             break;
          }
          default:
             break;
      }
      prev = (VkBaseInStructure *)current;
      current = current->pNext;
   }
}

static VkResult
wrapper_create_device_queue(struct wrapper_device *device,
                            const VkDeviceCreateInfo* pCreateInfo)
{
   const VkDeviceQueueCreateInfo *create_info;
   struct wrapper_queue *queue;
   VkResult result;

   for (int i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      create_info = &pCreateInfo->pQueueCreateInfos[i];
      for (int j = 0; j < create_info->queueCount; j++) {
         queue = vk_zalloc(&device->vk.alloc, sizeof(*queue), 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
         if (!queue)
            return VK_ERROR_OUT_OF_HOST_MEMORY;

         if (create_info->flags) {
            device->dispatch_table.GetDeviceQueue2(
               device->dispatch_handle,
               &(VkDeviceQueueInfo2) {
                  .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
                  .flags = create_info->flags,
                  .queueFamilyIndex = create_info->queueFamilyIndex,
                  .queueIndex = j,
               },
               &queue->dispatch_handle);;
         } else {
            device->dispatch_table.GetDeviceQueue(
               device->dispatch_handle, create_info->queueFamilyIndex,
               j, &queue->dispatch_handle);
         }
         queue->device = device;

         result = vk_queue_init(&queue->vk, &device->vk, create_info, j);
         if (result != VK_SUCCESS) {
            vk_free(&device->vk.alloc, queue);
            return result;
         }
      }
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateDevice(VkPhysicalDevice physicalDevice,
                     const VkDeviceCreateInfo* pCreateInfo,
                     const VkAllocationCallbacks* pAllocator,
                     VkDevice* pDevice)
{
   VK_FROM_HANDLE(wrapper_physical_device, physical_device, physicalDevice);
   const char *wrapper_enable_extensions[VK_DEVICE_EXTENSION_COUNT];
   uint32_t wrapper_enable_extension_count = 0;
   VkDeviceCreateInfo wrapper_create_info = *pCreateInfo;
   struct vk_device_dispatch_table dispatch_table;
   struct wrapper_device *device;
   VkPhysicalDeviceFeatures2 *pdf2;
   VkPhysicalDeviceFeatures *pdf;
   VkResult result;
   static int wrapper_safe_create_device = -1;

   /*
    * VK_KHR_buffer_device_address is mandatory for this wrapper (it
    * is relied on internally, not merely forwarded). Fail fast,
    * before allocating anything, if the driver can't actually back
    * it -- both the extension/core-version and the feature bit.
    */
   if (!physical_device->base_supported_extensions.KHR_buffer_device_address &&
       physical_device->properties2.properties.apiVersion < VK_API_VERSION_1_2) {
      WRAPPER_LOG(error, "Driver lacks mandatory VK_KHR_buffer_device_address");
      return vk_error(physical_device, VK_ERROR_EXTENSION_NOT_PRESENT);
   }

   if (!physical_device->base_supported_features.bufferDeviceAddress) {
      WRAPPER_LOG(error, "Driver lacks mandatory bufferDeviceAddress feature");
      return vk_error(physical_device, VK_ERROR_FEATURE_NOT_PRESENT);
   }

   /*
    * GPU/driver identification -- read straight off the device
    * (never assumed) -- decides whether this is a Mali Valhall part
    * with more than one shader core, in which case dynamic
    * rendering + multiview become mandatory too. See the "Mali
    * Valhall GPU / driver identification" block above.
    */
   bool wrapper_valhall_mandatory =
      wrapper_mali_requires_dynamic_rendering(physical_device);

   if (wrapper_valhall_mandatory) {
      VkResult valhall_result =
         wrapper_check_valhall_mandatory_extensions(physical_device);
      if (valhall_result != VK_SUCCESS)
         return vk_error(physical_device, valhall_result);
   }

   device = vk_zalloc2(&physical_device->instance->vk.alloc, pAllocator,
                       sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);

   list_inithead(&device->command_buffer_list);
   list_inithead(&device->device_memory_list);
   list_inithead(&device->image_list);
   list_inithead(&device->buffer_list);
   list_inithead(&device->fence_list);
   device->image_table = _mesa_hash_table_u64_create(NULL);
   device->buffer_table = _mesa_hash_table_u64_create(NULL);
   device->fence_table = _mesa_hash_table_u64_create(NULL);
   
   simple_mtx_init(&device->resource_mutex, mtx_plain);
   device->physical = physical_device;

   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wrapper_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wrapper_device_trampolines, false);

   result = vk_device_init(&device->vk, &physical_device->vk,
                           &dispatch_table, pCreateInfo, pAllocator);

   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to init Vulkan device, res %d", result);
      vk_free2(&physical_device->instance->vk.alloc, pAllocator,
               device);
      return vk_error(physical_device, result);
   }

   /* Required/owned extensions first, then everything else the
    * driver supports (minus filtered + known-buggy extensions), then
    * the conditionally-mandatory Valhall set. */
   wrapper_append_required_extensions(&device->vk,
      &wrapper_enable_extension_count, wrapper_enable_extensions);
   wrapper_enable_all_driver_extensions(device,
      &wrapper_enable_extension_count, wrapper_enable_extensions);

   if (wrapper_valhall_mandatory) {
      wrapper_append_valhall_extensions(
         &wrapper_enable_extension_count, wrapper_enable_extensions);
   }

   wrapper_create_info.enabledExtensionCount = wrapper_enable_extension_count;
   wrapper_create_info.ppEnabledExtensionNames = wrapper_enable_extensions;
   
   pdf = (void *)pCreateInfo->pEnabledFeatures;
   pdf2 = __vk_find_struct((void *)pCreateInfo->pNext,
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
            
#define DISABLE_FEATURE(f) \
if (pdf && pdf->f) { \
   pdf->f &= physical_device->base_supported_features.f; \
} \
\
if (pdf2 && pdf2->features.f) { \
   pdf2->features.f &= physical_device->base_supported_features.f; \
}

   DISABLE_FEATURE(textureCompressionBC);
   DISABLE_FEATURE(multiViewport);
   DISABLE_FEATURE(depthClamp);
   DISABLE_FEATURE(depthBiasClamp);
   DISABLE_FEATURE(fillModeNonSolid);
   DISABLE_FEATURE(shaderClipDistance);
   DISABLE_FEATURE(shaderCullDistance);
   DISABLE_FEATURE(dualSrcBlend);
   DISABLE_FEATURE(multiDrawIndirect);

#undef DISABLE_FEATURE

   process_pnext_chain((VkBaseInStructure *)&wrapper_create_info, device->physical);

   /* These storage structs must stay alive through the driver
    * CreateDevice() call below, so they live in this stack frame
    * rather than inside the helper functions. */
   VkPhysicalDeviceBufferDeviceAddressFeatures wrapper_bda_features = {0};
   wrapper_force_buffer_device_address(
      (VkBaseInStructure *)&wrapper_create_info, &wrapper_bda_features);

   VkPhysicalDeviceDynamicRenderingFeatures wrapper_dr_features = {0};
   VkPhysicalDeviceDynamicRenderingLocalReadFeatures wrapper_drlr_features = {0};
   VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT wrapper_dru_features = {0};
   VkPhysicalDeviceMultiviewFeatures wrapper_mv_features = {0};

   if (wrapper_valhall_mandatory) {
      wrapper_force_valhall_features(
         (VkBaseInStructure *)&wrapper_create_info,
         &wrapper_dr_features, &wrapper_drlr_features,
         &wrapper_dru_features, &wrapper_mv_features);
   }

   if (WRAPPER_LOG_LEVEL(info)) {
      for (int i = 0; i < wrapper_enable_extension_count; i++) {
         WRAPPER_LOG(info, "Enabling device extension %s", wrapper_enable_extensions[i]);
      }
   }

   if (wrapper_safe_create_device == -1) {
      wrapper_safe_create_device = getenv("WRAPPER_SAFE_CREATE_DEVICE") ? atoi(getenv("WRAPPER_SAFE_CREATE_DEVICE")) : 1;
   }
   
   result = physical_device->dispatch_table.CreateDevice(
      physical_device->dispatch_handle, &wrapper_create_info,
         pAllocator, &device->dispatch_handle);

   if (result != VK_SUCCESS) {
      if (wrapper_safe_create_device) {
         WRAPPER_LOG(info, "Forcing device creation with a NULL pNext chain");
         wrapper_create_info.pNext = NULL;
         result = physical_device->dispatch_table.CreateDevice(
            physical_device->dispatch_handle, &wrapper_create_info,
               pAllocator, &device->dispatch_handle);
      }
      
      if (result != VK_SUCCESS) {
         WRAPPER_LOG(error, "Failed driver createDevice, res %d", result);
         wrapper_DestroyDevice(wrapper_device_to_handle(device),
                               &device->vk.alloc);
         return vk_error(physical_device, result);
      }
   }

   void *gdpa = physical_device->instance->dispatch_table.GetInstanceProcAddr(
      physical_device->instance->dispatch_handle, "vkGetDeviceProcAddr");
   vk_device_dispatch_table_load(&device->dispatch_table, gdpa,
                                 device->dispatch_handle);

   result = wrapper_create_device_queue(device, pCreateInfo);
   if (result != VK_SUCCESS) {
      wrapper_DestroyDevice(wrapper_device_to_handle(device),
                            &device->vk.alloc);
      return vk_error(physical_device, result);
   }

   if (!physical_device->vk.supported_features.memoryMapPlaced) {
      device->vk.dispatch_table.AllocateMemory =
         wrapper_device_trampolines.AllocateMemory;
      device->vk.dispatch_table.MapMemory2 =
         wrapper_device_trampolines.MapMemory2;
      device->vk.dispatch_table.UnmapMemory =
         wrapper_device_trampolines.UnmapMemory;
      device->vk.dispatch_table.UnmapMemory2 =
         wrapper_device_trampolines.UnmapMemory2;
      device->vk.dispatch_table.FreeMemory =
         wrapper_device_trampolines.FreeMemory;
   }

   *pDevice = wrapper_device_to_handle(device);

   return VK_SUCCESS;
}

static void 
wrapper_buffer_destroy(struct wrapper_device *device,
					   struct wrapper_buffer *wb,
					   const VkAllocationCallbacks *pAllocator)
{
   if (wb == NULL)
      return;

   simple_mtx_lock(&device->resource_mutex);
      
   device->dispatch_table.DestroyBuffer(device->dispatch_handle,
      wb->dispatch_handle, pAllocator);

   _mesa_hash_table_u64_remove(device->buffer_table, (uint64_t)wb->dispatch_handle);
   list_del(&wb->link);

   simple_mtx_unlock(&device->resource_mutex);
   
   vk_object_free(&device->vk, &device->vk.alloc, wb);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateBuffer(VkDevice _device,
					 const VkBufferCreateInfo *pCreateInfo,
					 const VkAllocationCallbacks *pAllocator,
					 VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkResult res;

   res = device->dispatch_table.CreateBuffer(device->dispatch_handle,
      pCreateInfo, pAllocator, pBuffer);

   if (res != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to create buffer, res %d", res);
      return res;
   }

   simple_mtx_lock(&device->resource_mutex);

   struct wrapper_buffer *wb = vk_object_zalloc(&device->vk, 
      &device->vk.alloc, sizeof(struct wrapper_buffer), VK_OBJECT_TYPE_BUFFER);

   if (!wb) {
      WRAPPER_LOG(error, "Failed to allocate wrapper_buffer");
      simple_mtx_unlock(&device->resource_mutex);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
      
   wb->device = device;
   wb->size = pCreateInfo->size;
   wb->dispatch_handle = *pBuffer;

   list_add(&wb->link, &device->buffer_list);
   _mesa_hash_table_u64_insert(device->buffer_table, (uint64_t)wb->dispatch_handle, wb);

   simple_mtx_unlock(&device->resource_mutex);
   
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_BindBufferMemory(VkDevice _device,
						 VkBuffer buffer,
						 VkDeviceMemory memory,
						 VkDeviceSize memoryOffset)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkResult res;

   res = device->dispatch_table.BindBufferMemory(device->dispatch_handle,
      buffer, memory, memoryOffset);
   
   if (res != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to bind buffer memory, res %d", res);
      return res;
   }

   struct wrapper_buffer *wb = get_wrapper_buffer_from_handle(device, buffer);
   if (wb == NULL) {
      WRAPPER_LOG(error, "Failed to query wrapper_buffer");
      simple_mtx_unlock(&device->resource_mutex);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   wb->memory = memory;
   wb->offset = memoryOffset;

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyBuffer(VkDevice _device,
					  VkBuffer buffer,
					  const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   struct wrapper_buffer *wb = get_wrapper_buffer_from_handle(device, buffer);
   wrapper_buffer_destroy(device, wb, pAllocator);
}

static void 
wrapper_image_destroy(struct wrapper_device *device,
					  struct wrapper_image *wi,
					  const VkAllocationCallbacks *pAllocator)
{
   if (wi == NULL)
      return;

   simple_mtx_lock(&device->resource_mutex);
      
   device->dispatch_table.DestroyImage(device->dispatch_handle,
      wi->dispatch_handle, pAllocator);

   _mesa_hash_table_u64_remove(device->image_table, (uint64_t)wi->dispatch_handle);
   list_del(&wi->link);

   simple_mtx_unlock(&device->resource_mutex);
   
   vk_object_free(&device->vk, &device->vk.alloc, wi);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateImage(VkDevice _device,
					const VkImageCreateInfo *pCreateInfo,
					const VkAllocationCallbacks *pAllocator,
					VkImage *pImage)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkResult res;
   VkImageCreateInfo create_info = *pCreateInfo;

   if (is_emulated_bcn(device->physical, pCreateInfo->format)) {
      create_info.format = get_format_for_bcn(pCreateInfo->format);
      if (create_info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)
         create_info.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   }

   res = device->dispatch_table.CreateImage(device->dispatch_handle,
      &create_info, pAllocator, pImage);
   
   if (res != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to create image, res %d", res);
      return res;
   }

   simple_mtx_lock(&device->resource_mutex);

   struct wrapper_image *wi = vk_object_zalloc(&device->vk,
      &device->vk.alloc, sizeof(struct wrapper_image), VK_OBJECT_TYPE_IMAGE);

   if (!wi) {
      WRAPPER_LOG(error, "Failed to allocate wrapper_image");
      simple_mtx_unlock(&device->resource_mutex);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   wi->device = device;
   wi->info = *pCreateInfo;
   wi->dispatch_handle = *pImage;

   list_add(&wi->link, &device->image_list);
   _mesa_hash_table_u64_insert(device->image_table, (uint64_t)wi->dispatch_handle, wi);

   simple_mtx_unlock(&device->resource_mutex);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateImageView(VkDevice _device,
						const VkImageViewCreateInfo *pCreateInfo,
						const VkAllocationCallbacks *pAllocator,
						VkImageView *pView)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkImageViewCreateInfo create_info = *pCreateInfo;
   VkResult result;

   if (is_emulated_bcn(device->physical, pCreateInfo->format)) {
      create_info.format = get_format_for_bcn(pCreateInfo->format);
   }

   result = device->dispatch_table.CreateImageView(device->dispatch_handle,
     &create_info, pAllocator, pView);

   if (result != VK_SUCCESS)
   	  WRAPPER_LOG(error, "Failed to create image view, res %d", result);   	  

   return result;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyImage(VkDevice _device,
					 VkImage image,
					 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   struct wrapper_image *wi = get_wrapper_image_from_handle(device, image);
   wrapper_image_destroy(device, wi, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                       uint32_t queueIndex, VkQueue* pQueue) {
   vk_common_GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetDeviceQueue2(VkDevice _device, const VkDeviceQueueInfo2* pQueueInfo,
                        VkQueue* pQueue) {
   VK_FROM_HANDLE(vk_device, device, _device);

   struct vk_queue *queue = NULL;
   vk_foreach_queue(iter, device) {
      if (iter->queue_family_index == pQueueInfo->queueFamilyIndex &&
          iter->index_in_family == pQueueInfo->queueIndex &&
          iter->flags == pQueueInfo->flags) {
         queue = iter;
         break;
      }
   }

   *pQueue = queue ? vk_queue_to_handle(queue) : VK_NULL_HANDLE;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrapper_GetDeviceProcAddr(VkDevice _device, const char* pName) {
   VK_FROM_HANDLE(wrapper_device, device, _device);
   return vk_device_get_proc_addr(&device->vk, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_QueueSubmit(VkQueue _queue, uint32_t submitCount,
                    const VkSubmitInfo* pSubmits, VkFence fence)
{
   VK_FROM_HANDLE(wrapper_queue, queue, _queue);
   VkSubmitInfo wrapper_submits[submitCount];
   VkCommandBuffer *command_buffers;
   VkResult result;

   struct wrapper_fence *wf = get_wrapper_fence_from_handle(queue->device, fence);

   for (int i = 0; i < submitCount; i++) {
      const VkSubmitInfo *submit_info = &pSubmits[i];
      command_buffers = malloc(sizeof(VkCommandBuffer) *
         submit_info->commandBufferCount);
      for (int j = 0; j < submit_info->commandBufferCount; j++) {
         VK_FROM_HANDLE(wrapper_command_buffer, wcb,
                        submit_info->pCommandBuffers[j]);
         wcb->fence = wf;
         command_buffers[j] = wcb->dispatch_handle;
         
      }
      wrapper_submits[i] = pSubmits[i];
      wrapper_submits[i].pCommandBuffers = command_buffers;
   }

   result = queue->device->dispatch_table.QueueSubmit(
      queue->dispatch_handle, submitCount, wrapper_submits, fence);

   for (int i = 0; i < submitCount; i++)
      free((void *)wrapper_submits[i].pCommandBuffers);

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_QueueSubmit2(VkQueue _queue, uint32_t submitCount,
                     const VkSubmitInfo2* pSubmits, VkFence fence)
{
   VK_FROM_HANDLE(wrapper_queue, queue, _queue);
   VkSubmitInfo2 wrapper_submits[submitCount];
   VkCommandBufferSubmitInfo *command_buffers;
   VkResult result;

   struct wrapper_fence *wf = get_wrapper_fence_from_handle(queue->device, fence);

   for (int i = 0; i < submitCount; i++) {
      const VkSubmitInfo2 *submit_info = &pSubmits[i];
      command_buffers = malloc(sizeof(VkCommandBufferSubmitInfo) *
         submit_info->commandBufferInfoCount);
      for (int j = 0; j < submit_info->commandBufferInfoCount; j++) {
         VK_FROM_HANDLE(wrapper_command_buffer, wcb,
                        submit_info->pCommandBufferInfos[j].commandBuffer);
         wcb->fence = wf;
         command_buffers[j] = pSubmits[i].pCommandBufferInfos[j];
         command_buffers[j].commandBuffer = wcb->dispatch_handle;
      }
      wrapper_submits[i] = pSubmits[i];
      wrapper_submits[i].pCommandBufferInfos = command_buffers;
   }

   result = queue->device->dispatch_table.QueueSubmit2(
      queue->dispatch_handle, submitCount, wrapper_submits, fence);

   for (int i = 0; i < submitCount; i++)
      free((void *)wrapper_submits[i].pCommandBufferInfos);

   return result;
}

static void 
wrapper_fence_destroy(struct wrapper_device *device,
					  struct wrapper_fence *wf,
					  const VkAllocationCallbacks *pAllocator)
{
   if (wf == NULL)
      return;

   simple_mtx_lock(&device->resource_mutex);

   device->dispatch_table.DestroyFence(device->dispatch_handle,
      wf->dispatch_handle, pAllocator); 

   _mesa_hash_table_u64_remove(device->fence_table, (uint64_t)wf->dispatch_handle);
   list_del(&wf->link);
   
   simple_mtx_unlock(&device->resource_mutex);
   
   vk_object_free(&device->vk, &device->vk.alloc, wf);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateFence(VkDevice _device,
					const VkFenceCreateInfo *pCreateInfo,
					const VkAllocationCallbacks *pAllocator,
					VkFence *pFence)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   VkResult res = device->dispatch_table.CreateFence(device->dispatch_handle,
      pCreateInfo, pAllocator, pFence);

   if (res != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to create fence, res %d", res);
      return res;
   }

   simple_mtx_lock(&device->resource_mutex);
   
   struct wrapper_fence *wf = vk_object_zalloc(&device->vk,
      &device->vk.alloc, sizeof(struct wrapper_fence), VK_OBJECT_TYPE_FENCE);
   if (!wf) {
      WRAPPER_LOG(error, "Failed to allocate wrapper_fence");
      simple_mtx_unlock(&device->resource_mutex);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   wf->device = device;
   wf->dispatch_handle = *pFence;

   list_inithead(&wf->staging_buffers_list);

   list_add(&wf->link, &device->fence_list);
   _mesa_hash_table_u64_insert(device->fence_table, (uint64_t)wf->dispatch_handle, wf);

   simple_mtx_unlock(&device->resource_mutex);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_WaitForFences(VkDevice _device,
					  uint32_t fenceCount,
					  const VkFence *pFences,
					  VkBool32 waitAll,
					  uint64_t timeout)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkResult res;

   res = device->dispatch_table.WaitForFences(device->dispatch_handle,
     fenceCount, pFences, waitAll, timeout);

   if (res != VK_SUCCESS || device->physical->emulate_bcn < 2)
      return res;

   for (uint32_t i = 0; i < fenceCount; i++) {
      struct wrapper_fence *wf = get_wrapper_fence_from_handle(device, pFences[i]);
      list_for_each_entry_safe(struct wrapper_buffer, wb,
                               &wf->staging_buffers_list, link)
      {
         VkDeviceMemory memory = wb->memory;
         wrapper_buffer_destroy(device, wb, NULL);
         device->dispatch_table.FreeMemory(device->dispatch_handle,
            memory, NULL);
      }
   }

   return res;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyFence(VkDevice _device,
					 VkFence fence,
					 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   struct wrapper_fence *wf = get_wrapper_fence_from_handle(device, fence);
   wrapper_fence_destroy(device, wf, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                           uint32_t commandBufferCount,
                           const VkCommandBuffer* pCommandBuffers)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   VkCommandBuffer command_buffers[commandBufferCount];

   for (int i = 0; i < commandBufferCount; i++) {
      command_buffers[i] =
         wrapper_command_buffer_from_handle(pCommandBuffers[i])->dispatch_handle;
   }
   wcb->device->dispatch_table.CmdExecuteCommands(
      wcb->dispatch_handle, commandBufferCount, command_buffers);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_CreateShaderModule(VkDevice _device,
						   const VkShaderModuleCreateInfo *pCreateInfo,
						   const VkAllocationCallbacks *pAllocator,
						   VkShaderModule *pShaderModule)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   static int wrapper_no_remove_clip_distance = -1;
   static int wrapper_no_patch_OpConstComp = -1;

   if (wrapper_no_remove_clip_distance == -1)
      wrapper_no_remove_clip_distance = getenv("WRAPPER_NO_REMOVE_CLIP_DISTANCE") && atoi(getenv("WRAPPER_NO_REMOVE_CLIP_DISTANCE"));

   if (wrapper_no_patch_OpConstComp == -1)
      wrapper_no_patch_OpConstComp = getenv("WRAPPER_NO_PATCH_OPCONSTCOMP") && atoi(getenv("WRAPPER_NO_PATCH_OPCONSTCOMP"));
      
   VkShaderModuleCreateInfo create_info = *pCreateInfo;

   simple_mtx_lock(&device->resource_mutex);

   if (device->physical->driver_properties.driverID == VK_DRIVER_ID_ARM_PROPRIETARY) {
      uint32_t *code = malloc(create_info.codeSize);
      memcpy(code, create_info.pCode, create_info.codeSize);
      if (!wrapper_no_patch_OpConstComp) patch_OpConstantComposite_to_OpSpecConstantComposite(code, create_info.codeSize);
      if (!wrapper_no_remove_clip_distance) remove_ClipDistance(code, &create_info.codeSize);
      create_info.pCode = code;
   }

   simple_mtx_unlock(&device->resource_mutex);

   if (WRAPPER_LOG_LEVEL(shader))
      dump_shader_code(create_info.pCode, create_info.codeSize);
   
   return device->dispatch_table.CreateShaderModule(
      device->dispatch_handle, &create_info, pAllocator, pShaderModule);
}						   						   

static VkResult
wrapper_command_buffer_create(struct wrapper_device *device,
                              VkCommandPool pool,
                              VkCommandBuffer dispatch_handle,
                              VkCommandBuffer *pCommandBuffers) {
   struct wrapper_command_buffer *wcb;
   wcb = vk_object_zalloc(&device->vk, &device->vk.alloc,
                          sizeof(struct wrapper_command_buffer),
                          VK_OBJECT_TYPE_COMMAND_BUFFER);
   if (!wcb)
      return vk_error(&device->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   wcb->device = device;
   wcb->pool = pool;
   wcb->dispatch_handle = dispatch_handle;
   list_add(&wcb->link, &device->command_buffer_list);

   *pCommandBuffers = wrapper_command_buffer_to_handle(wcb);

   return VK_SUCCESS;
}

static void
wrapper_command_buffer_destroy(struct wrapper_device *device,
                               struct wrapper_command_buffer *wcb) {
   if (wcb == NULL)
      return;

   device->dispatch_table.FreeCommandBuffers(
      device->dispatch_handle, wcb->pool, 1, &wcb->dispatch_handle);

   list_del(&wcb->link);
   vk_object_free(&device->vk, &device->vk.alloc, wcb);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_AllocateCommandBuffers(VkDevice _device,
                               const VkCommandBufferAllocateInfo* pAllocateInfo,
                               VkCommandBuffer* pCommandBuffers)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);
   VkResult result;
   uint32_t i;
   
   result = device->dispatch_table.AllocateCommandBuffers(
      device->dispatch_handle, pAllocateInfo, pCommandBuffers);
   if (result != VK_SUCCESS)
      return result;

   simple_mtx_lock(&device->resource_mutex);

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      result = wrapper_command_buffer_create(
         device, pAllocateInfo->commandPool, pCommandBuffers[i],
         pCommandBuffers + i);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      for (int q = 0; q < i; q++) {
         VK_FROM_HANDLE(wrapper_command_buffer, wcb, pCommandBuffers[q]);
         wrapper_command_buffer_destroy(device, wcb);
      }

      device->dispatch_table.FreeCommandBuffers(
         device->dispatch_handle, pAllocateInfo->commandPool,
         pAllocateInfo->commandBufferCount - i, pCommandBuffers + i);
      
      for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
         pCommandBuffers[i] = VK_NULL_HANDLE;
      }
   }

   simple_mtx_unlock(&device->resource_mutex);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
wrapper_CmdCopyBufferToImage(VkCommandBuffer commandBuffer,
							 VkBuffer srcBuffer,
							 VkImage dstImage,
							 VkImageLayout dstLayout,
							 uint32_t regionCount,
							 const VkBufferImageCopy *pRegions)
{
   VK_FROM_HANDLE(wrapper_command_buffer, wcb, commandBuffer);
   VkResult res;

   struct wrapper_device *device = wcb->device;
   struct wrapper_image *wi = get_wrapper_image_from_handle(device, dstImage);   
   struct wrapper_buffer *wb = get_wrapper_buffer_from_handle(device, srcBuffer);
   VkFormat format = wi->info.format;
   int texel_size = get_texel_size_for_format(get_format_for_bcn(format));

   if (!wi || !wb || !is_emulated_bcn(device->physical, format)) {
      device->dispatch_table.CmdCopyBufferToImage(wcb->dispatch_handle,
         srcBuffer, dstImage, dstLayout, regionCount, pRegions);
      return;
   }

   simple_mtx_lock(&device->resource_mutex);
   
   if (!wb->is_mapped) {
      res = device->dispatch_table.MapMemory(device->dispatch_handle,
         wb->memory, wb->offset, wb->size, 0, &wb->mapped_address);
         
      if (res != VK_SUCCESS) {
         WRAPPER_LOG(error, "Failed to map source buffer memory, res %d", res);
         simple_mtx_unlock(&device->resource_mutex);
         return;
      }

      wb->is_mapped = 1;
   }
   
   for (int i = 0; i < regionCount; i++) {
      VkBufferImageCopy copy_region = pRegions[i];
      int w = copy_region.imageExtent.width;
      int h = copy_region.imageExtent.height;
      int offset = copy_region.bufferOffset;

      struct wrapper_buffer *staging_wb = vk_object_zalloc(&device->vk,
         &device->vk.alloc, sizeof(struct wrapper_buffer), VK_OBJECT_TYPE_BUFFER);

      VkBufferCreateInfo buffer_create_info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = w * h * texel_size,
         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
         .flags = 0,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };

      res = device->dispatch_table.CreateBuffer(device->dispatch_handle,
         &buffer_create_info, NULL, &staging_wb->dispatch_handle);

      if (res != VK_SUCCESS) {
         WRAPPER_LOG(error, "Failed to create staging buffer, res %d", res);
         simple_mtx_unlock(&device->resource_mutex);
         return;
      }

      VkMemoryAllocateInfo allocate_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = w * h * texel_size,
         .memoryTypeIndex = wrapper_select_device_memory_type(device,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
      };

      res = device->dispatch_table.AllocateMemory(device->dispatch_handle,
         &allocate_info, NULL, &staging_wb->memory);

      if (res != VK_SUCCESS) {
         WRAPPER_LOG(error, "Failed to allocate staging buffer memory, res %d", res);
         simple_mtx_unlock(&device->resource_mutex);
         return;
      }

      res = device->dispatch_table.BindBufferMemory(device->dispatch_handle,
         staging_wb->dispatch_handle, staging_wb->memory, 0);

      if (res != VK_SUCCESS) {
         WRAPPER_LOG(error, "Failed to bind staging buffer memory, res %d", res);
         simple_mtx_unlock(&device->resource_mutex);
         return;
      }

      res = device->dispatch_table.MapMemory(device->dispatch_handle,
         staging_wb->memory, 0, w * h * texel_size, 0, &staging_wb->mapped_address);

      if (res != VK_SUCCESS) {
         WRAPPER_LOG(error, "Failed to map staging buffer memory, res %d", res);
         simple_mtx_unlock(&device->resource_mutex);
         return;
      }

      decompress_bcn_format(wb->mapped_address, staging_wb->mapped_address, w, h, format, offset);
      
      copy_region.bufferOffset = 0;
      copy_region.bufferRowLength = 0;
      copy_region.bufferImageHeight = 0;

      device->dispatch_table.CmdCopyBufferToImage(wcb->dispatch_handle,
         staging_wb->dispatch_handle, dstImage, dstLayout, 1, &copy_region);

      staging_wb->wcb = wcb;
      staging_wb->device = device;

      if (wcb->fence)
         list_add(&staging_wb->link, &wcb->fence->staging_buffers_list);
   }

   if (wb->is_mapped) {
      device->dispatch_table.UnmapMemory(device->dispatch_handle,
         wb->memory);

      wb->is_mapped = 0;
   }

   simple_mtx_unlock(&device->resource_mutex);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_FreeCommandBuffers(VkDevice _device,
                           VkCommandPool commandPool,
                           uint32_t commandBufferCount,
                           const VkCommandBuffer* pCommandBuffers)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   simple_mtx_lock(&device->resource_mutex);

   for (int i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(wrapper_command_buffer, wcb, pCommandBuffers[i]);
      wrapper_command_buffer_destroy(device, wcb);
   }

   simple_mtx_unlock(&device->resource_mutex);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyCommandPool(VkDevice _device, VkCommandPool commandPool,
                           const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   simple_mtx_lock(&device->resource_mutex);

   list_for_each_entry_safe(struct wrapper_command_buffer, wcb,
                            &device->command_buffer_list, link) {
      if (wcb->pool == commandPool) {
         wrapper_command_buffer_destroy(device, wcb);
      }
   }

   simple_mtx_unlock(&device->resource_mutex);

   device->dispatch_table.DestroyCommandPool(device->dispatch_handle,
                                             commandPool, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_DestroyDevice(VkDevice _device, const VkAllocationCallbacks* pAllocator)
{
   VK_FROM_HANDLE(wrapper_device, device, _device);

   simple_mtx_lock(&device->resource_mutex);

   list_for_each_entry_safe(struct wrapper_command_buffer, wcb,
                            &device->command_buffer_list, link) {
      wrapper_command_buffer_destroy(device, wcb);
   }
   list_for_each_entry_safe(struct wrapper_device_memory, mem,
                            &device->device_memory_list, link) {
      wrapper_device_memory_destroy(mem);
   }
   
   simple_mtx_unlock(&device->resource_mutex);
   
   list_for_each_entry_safe(struct wrapper_buffer, wb,
                            &device->buffer_list, link) {
      wrapper_buffer_destroy(device, wb, pAllocator);
   }
   list_for_each_entry_safe(struct wrapper_image, wi,
                            &device->image_list, link) {
      wrapper_image_destroy(device, wi, pAllocator);
   }
   list_for_each_entry_safe(struct wrapper_fence, wf,
                            &device->fence_list, link) {
      wrapper_fence_destroy(device, wf, pAllocator);
   }

   list_for_each_entry_safe(struct vk_queue, queue, &device->vk.queues, link) {
      vk_queue_finish(queue);
      vk_free2(&device->vk.alloc, pAllocator, queue);
   }
   if (device->dispatch_handle != VK_NULL_HANDLE) {
      device->dispatch_table.DestroyDevice(device->
         dispatch_handle, pAllocator);
   }
   simple_mtx_destroy(&device->resource_mutex);
   vk_device_finish(&device->vk);
   vk_free2(&device->vk.alloc, pAllocator, device);
}

static uint64_t
unwrap_device_object(VkObjectType objectType,
                     uint64_t objectHandle)
{
   switch(objectType) {
   case VK_OBJECT_TYPE_DEVICE:
      return (uint64_t)(uintptr_t)wrapper_device_from_handle((VkDevice)(uintptr_t)objectHandle)->dispatch_handle;
   case VK_OBJECT_TYPE_QUEUE:
      return (uint64_t)(uintptr_t)wrapper_queue_from_handle((VkQueue)(uintptr_t)objectHandle)->dispatch_handle;
   case VK_OBJECT_TYPE_COMMAND_BUFFER:
      return (uint64_t)(uintptr_t)wrapper_command_buffer_from_handle((VkCommandBuffer)(uintptr_t)objectHandle)->dispatch_handle;
   default:
      return objectHandle;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_SetPrivateData(VkDevice _device, VkObjectType objectType,
                       uint64_t objectHandle,
                       VkPrivateDataSlot privateDataSlot,
                       uint64_t data) {
   VK_FROM_HANDLE(wrapper_device, device, _device);

   uint64_t object_handle = unwrap_device_object(objectType, objectHandle);
   return device->dispatch_table.SetPrivateData(device->dispatch_handle,
      objectType, object_handle, privateDataSlot, data);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPrivateData(VkDevice _device, VkObjectType objectType,
                       uint64_t objectHandle,
                       VkPrivateDataSlot privateDataSlot,
                       uint64_t* pData) {
   VK_FROM_HANDLE(wrapper_device, device, _device);

   uint64_t object_handle = unwrap_device_object(objectType, objectHandle);
   return device->dispatch_table.GetPrivateData(device->dispatch_handle,
      objectType, object_handle, privateDataSlot, pData);
}
