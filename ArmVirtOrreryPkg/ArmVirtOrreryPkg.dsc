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

## -----------------------------------------------------------------------------
## Shared drivers (OrreryPkg) — build-verification only. TpmProvisionApp/
## TpmVerifyBootApp hardcode an X64/OVMF flash address (see their own source
## comments); they compile clean here but are not wired up to run/boot on
## this platform yet. Any arm-virt-only drivers would go in
## ArmVirtOrreryPkg/Drivers instead.
## -----------------------------------------------------------------------------
[Components]
  OrreryPkg/Drivers/HelloDxe/HelloDxe.inf
  OrreryPkg/Drivers/HelloDxe2/HelloDxe2.inf
  OrreryPkg/Drivers/TpmProvisionApp/TpmProvisionApp.inf
  OrreryPkg/Drivers/TpmVerifyBootApp/TpmVerifyBootApp.inf
