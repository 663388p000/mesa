#include <vector>
#include <map>

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
   std::vector<uint32_t> code(pCode, pCode + (*codeSize / sizeof(uint32_t)));
   std::map<uint32_t, uint32_t> instructions;

   while (offset < code.size()) {
      uint32_t instruction = code[offset];
      uint32_t length = instruction >> 16;
      uint32_t opcode = instruction & 0xffffu;

      if (length == 0 || offset + length > code.size())
         break;

      if (opcode == OpCode::OpCapability) {
         uint32_t capability = code[offset + 1];
         if (capability == Capability::ClipDistance) {
            instructions.insert({offset, length});
         }
      }

      if (opcode == OpCode::OpDecorate) {
         uint32_t decoration = code[offset + 2];
         if (decoration == Decoration::Builtin) {
            uint32_t literal = code[offset + 3];
            if (literal == Decoration::Literals::ClipDistance) {
               instructions.insert({offset, length});
            }
         }
      }
      
      offset += length;
   }
 
   for (auto it = instructions.rbegin(); it != instructions.rend(); it++) {
      code.erase(code.begin() + it->first, code.begin() + it->first + it->second);
   }
   
   *codeSize = sizeof(uint32_t) * code.size();
   memcpy(pCode, code.data(), *codeSize);
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
