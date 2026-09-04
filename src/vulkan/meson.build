# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT

vk_api_xml = files('registry/vk.xml')

vulkan_icd_link_args = []
vulkan_icd_link_depends = []

if with_platform_android
  vulkan_icd_symbols = files('vulkan-icd-android-symbols.txt')

  if with_ld_version_script
    vulkan_icd_link_args += [
      '-Wl,--version-script',
      join_paths(meson.current_source_dir(), 'vulkan-android.sym')
    ]
    vulkan_icd_link_depends += files('vulkan-android.sym')
  endif
else
  vulkan_icd_symbols = files('vulkan-icd-symbols.txt')

  if with_ld_version_script
    vulkan_icd_link_args += [
      '-Wl,--version-script',
      join_paths(meson.current_source_dir(), 'vulkan.sym')
    ]
    vulkan_icd_link_depends += files('vulkan.sym')
  endif
endif

inc_vulkan_util = include_directories('util')

vulkan_wsi_deps = []
vulkan_wsi_list = []

vulkan_api_def = custom_target(
  'vulkan_api.def',
  input: 'vulkan_api.def.in',
  output: 'vulkan_api.def',
  command: gen_vs_module_defs_normal_command,
)

#
# Linux / X11 / XCB
#
if with_platform_x11
  vulkan_wsi_deps += [
    dep_xcb,
    dep_x11_xcb,
    dep_xcb_dri3,
    dep_xcb_present,
    dep_xcb_xfixes,
    dep_xcb_sync,
    dep_xcb_xrandr,
    dep_xcb_shm,
    dep_xshmfence,
  ]

  vulkan_wsi_list += [
    '-DVK_USE_PLATFORM_XCB_KHR',
    '-DVK_USE_PLATFORM_XLIB_KHR',
  ]

  if with_xcb_keysyms
    vulkan_wsi_deps += dep_xcb_keysyms
  endif
endif

#
# Android
#
if with_platform_android or with_wrapper_vk
  vulkan_wsi_list += '-DVK_USE_PLATFORM_ANDROID_KHR'
endif

#
# Wrapper only
#
idep_vulkan_wsi_defines = declare_dependency(
  compile_args: vulkan_wsi_list,
  dependencies: vulkan_wsi_deps,
)

vulkan_wsi_deps += idep_vulkan_wsi_defines

subdir('util')
subdir('runtime')
subdir('wsi')

if with_wrapper_vk
  subdir('wrapper')
endif
