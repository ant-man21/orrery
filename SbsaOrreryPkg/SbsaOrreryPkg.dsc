## =============================================================================
## SbsaOrreryPkg.dsc — QEMU 'sbsa-ref' (AArch64) platform description
##
## Strategy: include edk2-platforms' SbsaQemu.dsc in full, then append our
## own components. Mirrors ArmVirtOrreryPkg.dsc's / Q35Pkg.dsc's approach —
## except there is no upstream ArmVirtQemu-style "just add TPM2_ENABLE"
## story here: SbsaQemu.dsc ships with a plain (non-MM) VariableRuntimeDxe
## talking straight to non-secure NOR flash. Getting BL33 to route variable
## service calls through StandaloneMm in BL32 means swapping that stack out
## below — see docs/sbsa_boot_flow.md for the full BL1..BL33 handoff this
## sits on top of.
## =============================================================================

[Defines]
  PLATFORM_NAME                  = SbsaOrrery
  PLATFORM_GUID                  = c1f2a9d4-6e3b-4a7c-8f19-2d5b9e7a0c33
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = NOOPT|DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = edk2-platforms/Platform/Qemu/SbsaQemu/SbsaQemu.fdf

# Pull in all of SbsaQemu — library classes, PCDs, components, everything.
# (Also sets PLATFORM_NAME/OUTPUT_DIRECTORY -> Build/SbsaQemu; build.sh
# expects that path, not a SbsaOrrery-named one — see its comments.)
!include Platform/Qemu/SbsaQemu/SbsaQemu.dsc

[LibraryClasses.common]
  # Compat shim for QEMU < the version that added a "/cpus/topology" DT
  # node for sbsa-ref — see SbsaQemuHardwareInfoLibCompat.c for the full
  # story (upstream's version calls ResetShutdown() on this QEMU, which
  # looks exactly like a boot hang on the serial console).
  HardwareInfoLib|OrreryPkg/Library/SbsaQemuHardwareInfoLibCompat/SbsaQemuHardwareInfoLibCompat.inf

  # SbsaQemu.dsc has no TPM2_ENABLE story of its own (unlike ArmVirtQemu.dsc
  # / OvmfPkg, which both define these) — TpmProvisionApp/TpmVerifyBootApp
  # need Tpm2CommandLib directly.
  Tpm2CommandLib|SecurityPkg/Library/Tpm2CommandLib/Tpm2CommandLib.inf
  Tpm2HelpLib|SecurityPkg/Library/Tpm2HelpLib/Tpm2HelpLib.inf

[LibraryClasses.common.UEFI_APPLICATION]
  Tpm2DeviceLib|SecurityPkg/Library/Tpm2DeviceLibTcg2/Tpm2DeviceLibTcg2.inf
  Tpm2PolicyPcrLib|OrreryPkg/Library/Tpm2PolicyPcrLib/Tpm2PolicyPcrLib.inf
  Tpm2PcrLib|OrreryPkg/Library/Tpm2PcrLib/Tpm2PcrLib.inf
  PlatformRomInfoLib|OrreryPkg/Library/PlatformRomInfoLibSbsaQemu/PlatformRomInfoLibSbsaQemu.inf
  Tpm2PolicyAuthorizeLib|OrreryPkg/Library/Tpm2PolicyAuthorizeLib/Tpm2PolicyAuthorizeLib.inf

## -----------------------------------------------------------------------------
## Shared drivers (OrreryPkg). Same TPM measured-boot demo apps as the other
## two platforms, driven by PlatformRomInfoLib as above.
## -----------------------------------------------------------------------------
[Components]
  OrreryPkg/Drivers/HelloDxe/HelloDxe.inf
  OrreryPkg/Drivers/HelloDxe2/HelloDxe2.inf
  OrreryPkg/Drivers/TpmProvisionApp/TpmProvisionApp.inf
  OrreryPkg/Drivers/TpmVerifyBootApp/TpmVerifyBootApp.inf
