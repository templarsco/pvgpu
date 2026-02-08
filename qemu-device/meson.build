# PVGPU - Paravirtualized GPU device for QEMU
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This file should be included from hw/display/meson.build in the QEMU source tree.
# 
# To integrate pvgpu into QEMU:
# 1. Copy qemu-device/pvgpu.c and qemu-device/pvgpu.h to hw/display/ in QEMU source
# 2. Copy protocol/pvgpu_protocol.h to include/hw/display/ in QEMU source
# 3. Add the following line to hw/display/meson.build:
#    softmmu_ss.add(when: 'CONFIG_PVGPU', if_true: files('pvgpu.c'))
# 4. Add 'config_pvgpu' option to hw/display/Kconfig:
#    config PVGPU
#        bool
#        default y
#        depends on PCI
# 5. Rebuild QEMU with: meson setup build && ninja -C build

# Standalone meson.build for reference/testing
# This can be used to verify the file compiles outside of QEMU tree

project('pvgpu', 'c',
  version : '0.1.0',
  default_options : ['warning_level=2', 'c_std=c11']
)

# When building as part of QEMU, this file is not used directly.
# Instead, QEMU's build system includes our source file.

# For standalone syntax checking (not functional without QEMU headers):
# meson setup build -Dstandalone_check=true

standalone_check = get_option('standalone_check')

if standalone_check
  # Create stub definitions for syntax checking
  add_project_arguments('-DPVGPU_STANDALONE_CHECK', language: 'c')
  
  # This won't link but can verify basic syntax
  executable('pvgpu_check',
    sources: ['pvgpu.c'],
    include_directories: ['../protocol'],
    c_args: ['-fsyntax-only'],
    build_by_default: false
  )
endif
