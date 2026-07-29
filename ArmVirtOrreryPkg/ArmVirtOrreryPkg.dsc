## =============================================================================
## ArmVirtOrreryPkg.dsc — QEMU 'virt' (AArch64) platform description
##
## Strategy: include ArmVirtQemu.dsc in full, then append our own components.
## ArmVirtQemu.dsc defines all library classes, PCDs, and base components,
## including its own TPM2_ENABLE-gated TCG2 support (same shape as OVMF's).
## Mirrors Q35Pkg.dsc's approach for the X64/Q35 target.
## =============================================================================

[Defines]
  PLATFORM_NAME                  = ArmVirtOrrery
  PLATFORM_GUID                  = 66266d63-b5c9-4cf8-a063-7bdb7b4cf9e2
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/ArmVirtQemu-AArch64
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = NOOPT|DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = edk2/ArmVirtPkg/ArmVirtQemu.fdf

# Pull in all of ArmVirtQemu — library classes, PCDs, components, everything.
# Our additions come after so they can use everything it already defined.
!include ArmVirtPkg/ArmVirtQemu.dsc

[LibraryClasses.common.UEFI_APPLICATION]
  Tpm2DeviceLib|SecurityPkg/Library/Tpm2DeviceLibTcg2/Tpm2DeviceLibTcg2.inf
  Tpm2PolicyPcrLib|OrreryPkg/Library/Tpm2PolicyPcrLib/Tpm2PolicyPcrLib.inf
  Tpm2PcrLib|OrreryPkg/Library/Tpm2PcrLib/Tpm2PcrLib.inf
  PlatformRomInfoLib|OrreryPkg/Library/PlatformRomInfoLibArmVirt/PlatformRomInfoLibArmVirt.inf
  Tpm2PolicyAuthorizeLib|OrreryPkg/Library/Tpm2PolicyAuthorizeLib/Tpm2PolicyAuthorizeLib.inf

## -----------------------------------------------------------------------------
## Shared drivers (OrreryPkg). TpmProvisionApp/TpmVerifyBootApp get their
## flash-ROM location from PlatformRomInfoLib (see above), not a hardcoded
## address or runtime discovery — see docs/rom_discovery_story.md for why.
## Any arm-virt-only drivers would go in ArmVirtOrreryPkg/Drivers instead.
## -----------------------------------------------------------------------------
[Components]
  OrreryPkg/Drivers/HelloDxe/HelloDxe.inf
  OrreryPkg/Drivers/HelloDxe2/HelloDxe2.inf
  OrreryPkg/Drivers/TpmProvisionApp/TpmProvisionApp.inf
  OrreryPkg/Drivers/TpmVerifyBootApp/TpmVerifyBootApp.inf
