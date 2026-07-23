## =============================================================================
## Q35Pkg.dsc — Q35 platform description
##
## Strategy: include OvmfPkgX64.dsc in full, then append our own components.
## OvmfPkgX64.dsc defines all library classes, PCDs, and base components.
## We add our drivers in a second [Components] block after the include.
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

[LibraryClasses.common.UEFI_APPLICATION]
  Tpm2DeviceLib|SecurityPkg/Library/Tpm2DeviceLibTcg2/Tpm2DeviceLibTcg2.inf
  Tpm2PolicyPcrLib|Q35Pkg/Library/Tpm2PolicyPcrLib/Tpm2PolicyPcrLib.inf
  Tpm2PcrLib|Q35Pkg/Library/Tpm2PcrLib/Tpm2PcrLib.inf

## -----------------------------------------------------------------------------
## Q35Pkg drivers — add new INFs here as you build them
## -----------------------------------------------------------------------------
[Components]
  Q35Pkg/Drivers/HelloDxe/HelloDxe.inf
  Q35Pkg/Drivers/HelloDxe2/HelloDxe2.inf
  Q35Pkg/Drivers/TpmProvisionApp/TpmProvisionApp.inf
  Q35Pkg/Drivers/TpmVerifyBootApp/TpmVerifyBootApp.inf