#include <vector>

#include "spirv_patcher.hpp"
#include "wrapper_log.h"

namespace OpCode {
   static const uint32_t OpSpecConstantTrue = 48;
   static const uint32_t OpConstantComposite = 44;
   static const uint32_t OpSpecConstantComposite = 51;
}

void 
patch_OpConstantComposite_to_OpSpecConstantComposite(uint32_t *pCode, uint32_t codeSize)
{
   std::vector <uint32_t> true_bool_constants;
   uint32_t offset = 5;

   while (offset < codeSize) {
      uint32_t instruction = pCode[offset];
      uint32_t length = instruction >> 16;
      uint32_t opcode = instruction & 0xffffu;

      if (length == 0 || offset + length > codeSize)
         break;

      if (opcode == OpCode::OpSpecConstantTrue) 
         true_bool_constants.push_back(pCode[offset + 2]);

      if (opcode == OpCode::OpConstantComposite) {
         uint32_t component = pCode[offset + 3];
         if (std::find(true_bool_constants.begin(), true_bool_constants.end(), component) != true_bool_constants.end()) 
            pCode[offset] = (pCode[offset] & ~0xffffu) | (OpCode::OpSpecConstantComposite & 0xffffu);
      }
         
      offset += length;
   }
}
