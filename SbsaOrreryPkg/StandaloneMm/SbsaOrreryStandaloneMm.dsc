## @file
# SbsaOrreryStandaloneMm.dsc — BL32 for QEMU sbsa-ref (AArch64)
#
# Builds StandaloneMm.fd: a Secure-EL0 partition (StandaloneMmPkg core +
# ArmPkg's StandaloneMmCpu + a NOR-flash-backed variable store) that TF-A
# dispatches into via SPM_MM. This becomes BL32 when trusted-firmware-a is
# built with SPM_MM=1 BL32=<this .fd>.
#
# There is no upstream "SbsaQemu StandaloneMm" DSC in edk2-platforms (its
# Readme.md only documents BL1/BL2/BL31 + BL33 — no BL32 at all). This DSC
# is Orrery's own, modeled on edk2-platforms/Platform/ARM/JunoPkg's
# PlatformStandaloneMm.dsc (the closest real, working SPM_MM reference —
# see docs/sbsa_boot_flow.md for how the addresses below were derived from
# trusted-firmware-a/plat/qemu/qemu_sbsa/include/platform_def.h and
# plat/qemu/common/qemu_spm.c).
#
# Copyright (c) 2026, Orrery Project.
#
#    SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = SbsaOrreryStandaloneMm
  PLATFORM_GUID                  = 3a2f9c9e-6b8d-4b1a-9f7f-3f6b6e1a2c40
  PLATFORM_VERSION                = 0.1
  DSC_SPECIFICATION              = 0x0001001C
  OUTPUT_DIRECTORY               = Build/SbsaOrreryStandaloneMm
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = SbsaOrreryPkg/StandaloneMm/SbsaOrreryStandaloneMm.fdf

  DEFINE ENABLE_UEFI_SECURE_VARIABLE = TRUE
  DEFINE ENABLE_TPM                  = FALSE
  DEFINE COMPRESSION_TOOL_GUID       = D42AE6BD-1352-4bfb-909A-CA72A6EAE889

!include Platform/ARM/VExpressPkg/PlatformStandaloneMm.dsc.inc

################################################################################
#
# Library Class section — platform-specific overrides.
#
################################################################################
[LibraryClasses]
  # PL011 secure UART (UART2_BASE in TF-A's qemu_sbsa platform_def.h) —
  # kept separate from BL31's crash console (UART1) and BL33's boot
  # console (UART0) so StMM's own prints land on their own -serial line.
  NorFlashDeviceLib|Platform/ARM/Library/P30NorFlashDeviceLib/P30NorFlashDeviceLib.inf
  NorFlashPlatformLib|SbsaOrreryPkg/StandaloneMm/Library/NorFlashSbsaQemuLib/NorFlashSbsaQemuLib.inf

[PcdsFixedAtBuild]
!if $(TARGET) == RELEASE
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x21
!else
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2f
!endif

  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8000004F
  gEfiMdePkgTokenSpaceGuid.PcdDebugClearMemoryValue|0xAF
  gEfiMdePkgTokenSpaceGuid.PcdMaximumGuidedExtractHandler|0x2

  ## PL011 - Secure Terminal (UART2_BASE, see platform_def.h)
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterBase|0x60040000
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultReceiveFifoDepth|0
  gArmPlatformTokenSpaceGuid.PL011UartClkInHz|1
  gArmPlatformTokenSpaceGuid.PL011UartInterrupt|0

  #
  # NV Storage PCDs — see NorFlashSbsaQemuLib.c for the flash layout this
  # slices up. QEMU_SECURE_VARSTORE_BASE (0x01000000) is TF-A's fixed
  # secure-flash window for StMM's own NV storage.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase|0x01000000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x00010000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase|0x01010000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingSize|0x00010000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase|0x01020000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareSize|0x00010000

  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxAuthVariableSize|0x2800

  # Legacy SPM_MM dispatch conduit (SMC via TF-A's spm_mm service), not the
  # newer FF-A SPMC path — matches trusted-firmware-a's SPM_MM=1 build.
  gEfiMdeModulePkgTokenSpaceGuid.PcdFfaLibConduitSmc|FALSE

  #
  # The BFV is not located in flash but loaded into RAM by TF-A/BL2
  # directly (see PLAT_QEMU_SP_IMAGE_BASE in platform_def.h), so no shadow
  # copy is needed while loading StMM drivers.
  #
  gStandaloneMmPkgTokenSpaceGuid.PcdShadowBfv|FALSE

###################################################################################################
[Components.common]

###################################################################################################
[BuildOptions.AARCH64]
  GCC:*_*_*_DLINK_FLAGS = -z common-page-size=0x1000 -march=armv8-a+nofp -mstrict-align
  GCC:*_*_*_CC_FLAGS = -mstrict-align
!if $(ENABLE_UEFI_SECURE_VARIABLE) == TRUE
  GCC:*_*_*_CC_FLAGS = -DENABLE_UEFI_SECURE_VARIABLE
!endif
