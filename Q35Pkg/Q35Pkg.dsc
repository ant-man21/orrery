## =============================================================================
## Q35Pkg.dsc — Q35 platform description
##
## Strategy: include OvmfPkgX64.dsc in full, then append our own components.
## OvmfPkgX64.dsc defines all library classes, PCDs, and base components.
## Our drivers (shared ones from OrreryPkg, plus any q35-only ones) go in a
## second [Components] block after the include.
## =============================================================================

[Defines]
  PLATFORM_NAME                  = Q35
  PLATFORM_GUID                  = 7a1d5e2f-3b4c-4a8e-9f0d-1c2e3a4b5d6f
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/OvmfX64
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = NOOPT|DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = edk2/OvmfPkg/OvmfPkgX64.fdf

# Pull in all of OVMF — library classes, PCDs, components, everything.
# Our additions come after so they can use everything OVMF already defined.
!include OvmfPkg/OvmfPkgX64.dsc

[PcdsFixedAtBuild]
  # OvmfPkgX64.dsc sets this to 0xFFFFFFFF -- every DEBUG() class enabled,
  # including DEBUG_POOL (0x10) / DEBUG_PAGE (0x20), which trace every
  # single AllocatePool/FreePool/ConvertRange/AddRange call. On a DEBUG
  # build that's 100k+ lines of noise in debug.log with no boot-relevant
  # signal in it. Use OVMF's own commented-out "reasonable" suggestion
  # instead: DEBUG_INIT|DEBUG_WARN|DEBUG_LOAD|DEBUG_FS|DEBUG_INFO|DEBUG_ERROR.
  # SEC:/PEI/DXE driver-load/BdsDxe phase markers all stay; pool/page
  # tracing goes.
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8000004F

[LibraryClasses.common.UEFI_APPLICATION]
  Tpm2DeviceLib|SecurityPkg/Library/Tpm2DeviceLibTcg2/Tpm2DeviceLibTcg2.inf
  Tpm2PolicyPcrLib|OrreryPkg/Library/Tpm2PolicyPcrLib/Tpm2PolicyPcrLib.inf
  Tpm2PcrLib|OrreryPkg/Library/Tpm2PcrLib/Tpm2PcrLib.inf
  PlatformRomInfoLib|OrreryPkg/Library/PlatformRomInfoLibQ35/PlatformRomInfoLibQ35.inf
  Tpm2PolicyAuthorizeLib|OrreryPkg/Library/Tpm2PolicyAuthorizeLib/Tpm2PolicyAuthorizeLib.inf

## -----------------------------------------------------------------------------
## Shared drivers (OrreryPkg) + any q35-only drivers (Q35Pkg/Drivers, none yet)
## -----------------------------------------------------------------------------
[Components]
  OrreryPkg/Drivers/HelloDxe/HelloDxe.inf
  OrreryPkg/Drivers/HelloDxe2/HelloDxe2.inf
  OrreryPkg/Drivers/TpmProvisionApp/TpmProvisionApp.inf
  OrreryPkg/Drivers/TpmVerifyBootApp/TpmVerifyBootApp.inf