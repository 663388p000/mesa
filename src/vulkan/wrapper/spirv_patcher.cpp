#include <vector>

#include "spirv_patcher.hpp"
#include "wrapper_log.h"

void 
patch_OpSelect_for_unbound_textures(uint32_t *pCode, uint32_t codeSize)
{
   uint32_t offset = 5;
   uint32_t constant_id;
   uint32_t vector_constant_id;

   while (offset < codeSize) {
      uint32_t instruction = pCode[offset];
      uint32_t length = instruction >> 16;
      uint32_t opcode = instruction & 0xffffu;

      if (length == 0 || offset + length > codeSize)
         break;

      if (opcode == 43) {
         uint32_t result_type_id = pCode[offset + 1];
         uint32_t result_id = pCode[offset + 2];
         uint32_t value = pCode[offset + 3];
         if (value == 0)
            constant_id = result_id;
      }

      if (opcode == 44) {
         uint32_t result_type_id = pCode[offset + 1];
         uint32_t result_id = pCode[offset + 2];
         int constituents_len = length - 3;
         if (constituents_len == 4) {
            int components = 0;
            for (int i = 3; i < length; i++) {
               if (pCode[offset + i] == constant_id)
                  components++;
            }
            if (components == 4) {
               vector_constant_id = result_id;
            }
         }
      }

      if (opcode == 169) {
         uint32_t result_type_id = pCode[offset + 1];
         uint32_t result_id = pCode[offset + 2];
         uint32_t condition_id = pCode[offset + 3];
         uint32_t object1_id = pCode[offset + 4];
         uint32_t object2_id = pCode[offset + 5];
         if (object2_id == vector_constant_id) {
            WRAPPER_LOG(info, "Patching OpSelect for unbound textures");
            pCode[offset + 5] = pCode[offset + 4];
         }
      }

      offset += length;
   }
}
