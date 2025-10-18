#ifndef __SPIRV_PATCHER_HPP
#define __SPIRV_PATCHER_HPP

#ifdef __cplusplus
extern "C" {
#endif

void
patch_OpSelect_for_unbound_textures(uint32_t *pCode, uint32_t codeSize);

#ifdef __cplusplus
}
#endif

#endif
