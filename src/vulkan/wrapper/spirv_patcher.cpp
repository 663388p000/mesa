#include <vector>

#include "spirv_patcher.hpp"
#include "wrapper_log.h"

namespace Decoration {
   static const uint32_t Builtin = 11;

   namespace Literals {
      static const uint32_t ClipDistance = 3;
   }
}

namespace Capability {
   static const uint32_t ClipDistance = 32;
}

namespace OpCode {
   static const uint32_t OpCapability = 17;
   static const uint32_t OpConstantComposite = 44;
   static const uint32_t OpSpecConstantTrue = 48;
   static const uint32_t OpSpecConstantComposite = 51;
   static const uint32_t OpDecorate = 71;
}

void
remove_ClipDistance(uint32_t *pCode, size_t *codeSize)
{
   uint32_t offset = 5;
   std::vector<uint32_t> patched_code(pCode, pCode + (*codeSize / sizeof(uint32_t)));

   while (offset < *codeSize) {
      uint32_t instruction = pCode[offset];
      uint32_t length = instruction >> 16;
      uint32_t opcode = instruction & 0xffffu;

      if (length == 0 || offset + length > *codeSize)
         break;

      if (opcode == OpCode::OpCapability) {
         uint32_t capability = pCode[offset + 1];
         if (capability == Capability::ClipDistance) {
            WRAPPER_LOG(info, "Removing OpCapability ClipDistance");
            patched_code.erase(patched_code.begin() + offset, patched_code.begin() + offset + length);
         }
      }

      if (opcode == OpCode::OpDecorate) {
         uint32_t decoration = pCode[offset + 2];
         uint32_t literal = pCode[offset + 3];
         if (decoration == Decoration::Builtin && literal == Decoration::Literals::ClipDistance) {
            WRAPPER_LOG(info, "Removing OpDecorate ClipDistance");
            patched_code.erase(patched_code.begin() + offset, patched_code.begin() + offset + length - 1);
         }
      }
      
      offset += length;
   }
   
   *codeSize = sizeof(uint32_t) * patched_code.size();
   memcpy(pCode, patched_code.data(), *codeSize);
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
