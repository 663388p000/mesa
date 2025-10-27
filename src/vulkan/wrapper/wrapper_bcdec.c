#include "wrapper_bcdec.h"
#include "wrapper_log.h"
#include <time.h>

#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION

#include "bcdec.h"

VkFormat 
get_format_for_bcn(VkFormat bcn_format)
{
   switch(bcn_format) {
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      case VK_FORMAT_BC2_SRGB_BLOCK:
      case VK_FORMAT_BC3_SRGB_BLOCK:
      case VK_FORMAT_BC7_SRGB_BLOCK:
         return VK_FORMAT_R8G8B8A8_SRGB;
      case VK_FORMAT_BC4_UNORM_BLOCK:
         return VK_FORMAT_R8_UNORM;
      case VK_FORMAT_BC4_SNORM_BLOCK:
         return VK_FORMAT_R8_SNORM;
      case VK_FORMAT_BC5_UNORM_BLOCK:
          return VK_FORMAT_R8G8_UNORM;
      case VK_FORMAT_BC5_SNORM_BLOCK:
         return VK_FORMAT_R8G8_SNORM;
      case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      case VK_FORMAT_BC6H_UFLOAT_BLOCK:
         return VK_FORMAT_R16G16B16_SFLOAT;
      default:
         return VK_FORMAT_R8G8B8A8_UNORM;
   }
}

int 
get_texel_size_for_format(VkFormat format) 
{
   switch (format) {
      case VK_FORMAT_R16G16B16_SFLOAT:
         return 6;
      case VK_FORMAT_R8G8_UNORM:
      case VK_FORMAT_R8G8_SNORM:
         return 2;
      case VK_FORMAT_R8_UNORM:
      case VK_FORMAT_R8_SNORM:
         return 1;
      default:
         return 4;
   }
}

int
is_emulated_bcn(struct wrapper_physical_device *pdev, VkFormat format)
{
   switch(format) {
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      case VK_FORMAT_BC2_SRGB_BLOCK:
      case VK_FORMAT_BC2_UNORM_BLOCK:
      case VK_FORMAT_BC3_UNORM_BLOCK:
      case VK_FORMAT_BC3_SRGB_BLOCK:
         if (pdev->emulate_bcn == 3 && 
             pdev->driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY)
         {
            return 0;
         }
         else if (pdev->emulate_bcn > 1) {
            return 1;
         } else {
            return 0;
         }
         break;
      case VK_FORMAT_BC4_UNORM_BLOCK:
      case VK_FORMAT_BC4_SNORM_BLOCK:
      case VK_FORMAT_BC5_SNORM_BLOCK:
      case VK_FORMAT_BC5_UNORM_BLOCK:
      case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      case VK_FORMAT_BC6H_UFLOAT_BLOCK:
      case VK_FORMAT_BC7_SRGB_BLOCK:
      case VK_FORMAT_BC7_UNORM_BLOCK:
         if (pdev->emulate_bcn > 1)
            return 1;
         else
            return 0;
         break;
      default:
         return 0;
   }
}

void 
decompress_bcn_format(void *srcBuffer,
					  void *dstBuffer,
					  int w,
					  int h,
					  VkFormat format,
					  int offset)
{
   char *src = srcBuffer + offset;
   char *dst = dstBuffer;
   clock_t start, end;
   static int wrapper_mark_bcn =  -1;

   if (wrapper_mark_bcn == -1)
      wrapper_mark_bcn = getenv("WRAPPER_MARK_BCN") && atoi(getenv("WRAPPER_MARK_BCN"));

   int texel_size = get_texel_size_for_format(get_format_for_bcn(format));

   if (WRAPPER_LOG_LEVEL(info))
      start = clock();

   for (int i = 0; i < h; i += 4) {
      for (int j = 0; j < w; j += 4) {
         dst = dstBuffer + (i * w * texel_size) + j * texel_size;
         switch (format) {
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGB_SRGB_BLOCK:                                           
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:                                          
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
               bcdec_bc1(src, dst, w * texel_size);
               src += BCDEC_BC1_BLOCK_SIZE;
               break;
            case VK_FORMAT_BC2_SRGB_BLOCK:
            case VK_FORMAT_BC2_UNORM_BLOCK:
               bcdec_bc2(src, dst, w * texel_size);
               src += BCDEC_BC2_BLOCK_SIZE;
               break;
            case VK_FORMAT_BC3_UNORM_BLOCK:
            case VK_FORMAT_BC3_SRGB_BLOCK:
               bcdec_bc3(src, dst, w * texel_size);
               src += BCDEC_BC3_BLOCK_SIZE;
               break;
            case VK_FORMAT_BC4_UNORM_BLOCK:
            case VK_FORMAT_BC4_SNORM_BLOCK:
               bcdec_bc4(src, dst, w * texel_size, format == VK_FORMAT_BC4_SNORM_BLOCK);
               src += BCDEC_BC4_BLOCK_SIZE;
               break;
            case VK_FORMAT_BC5_SNORM_BLOCK:                                              
            case VK_FORMAT_BC5_UNORM_BLOCK:
               bcdec_bc5(src, dst, w * texel_size, format == VK_FORMAT_BC5_SNORM_BLOCK);
               src += BCDEC_BC5_BLOCK_SIZE;
               break;
            case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            case VK_FORMAT_BC6H_UFLOAT_BLOCK:
               bcdec_bc6h_half(src, dst, w * texel_size, format == VK_FORMAT_BC6H_SFLOAT_BLOCK);
               src += BCDEC_BC6H_BLOCK_SIZE;
               break;
            case VK_FORMAT_BC7_SRGB_BLOCK:                                               
            case VK_FORMAT_BC7_UNORM_BLOCK:
               bcdec_bc7(src, dst, w * texel_size);
               src += BCDEC_BC7_BLOCK_SIZE;
               break;
            default:
               break;
         }
      }
   }

   if (WRAPPER_LOG_LEVEL(info)) {
      end = clock();
      double elapsed_time = (((double)(end - start)) / CLOCKS_PER_SEC) * 1000.0;
      WRAPPER_LOG(info, "Raw texture data decompressed in %fms", elapsed_time);
   }

   if (wrapper_mark_bcn) {
      WRAPPER_LOG(info, "Marking BCn texture");
      for (int i = 0; i < w; i++) {
         switch(format) {
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGB_SRGB_BLOCK:                                          
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:                                          
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            /* Yellow */
               dst[i * texel_size] = 0xFF;
               dst[i * texel_size + 1] = 0xFF;
               dst[i * texel_size + 2] = 0;
               dst[i * texel_size + 3] = 255;
               break;
            case VK_FORMAT_BC2_SRGB_BLOCK:
            case VK_FORMAT_BC2_UNORM_BLOCK:
            /* Blue */
               dst[i * texel_size] = 0;
               dst[i * texel_size + 1] = 0;
               dst[i * texel_size + 2] = 0xFF;
               dst[i * texel_size + 3] = 255;
               break;
            case VK_FORMAT_BC3_UNORM_BLOCK:
            case VK_FORMAT_BC3_SRGB_BLOCK:
            /* Light Blue */
               dst[i * texel_size] = 0;
               dst[i * texel_size + 1] = 0xFF;
               dst[i * texel_size + 2] = 0xFF;
               dst[i * texel_size + 3] = 255;
               break;
            case VK_FORMAT_BC4_UNORM_BLOCK:
            case VK_FORMAT_BC4_SNORM_BLOCK:
            /* Red */
               dst[i * texel_size] = 0xFF;
               break;
            case VK_FORMAT_BC5_UNORM_BLOCK:
            case VK_FORMAT_BC5_SNORM_BLOCK:
            /* Green */
               dst[i * texel_size] = 0;
               dst[i * texel_size + 1] = 0xFF;
               break;
            case VK_FORMAT_BC6H_SFLOAT_BLOCK:                                            
            case VK_FORMAT_BC6H_UFLOAT_BLOCK:
            /* Purple */
               dst[i * texel_size] = 0x90;
               dst[i * texel_size + 1] = 0x40;
               dst[i * texel_size + 2] = 0xA0;
               break;
            case VK_FORMAT_BC7_UNORM_BLOCK:
            case VK_FORMAT_BC7_SRGB_BLOCK:
            /* Black */
               dst[i * texel_size] = 0xFF;                                                  
               dst[i * texel_size + 1] = 0xFF;                                              
               dst[i * texel_size + 2] = 0xFF;                                                 
               dst[i * texel_size + 3] = 255;
               break;
            default:
               break;
         }
      }
   }
}
