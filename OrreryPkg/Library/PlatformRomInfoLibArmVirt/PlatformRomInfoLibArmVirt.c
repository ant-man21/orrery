/** @file
  PlatformRomInfoLib instance for ArmVirtQemu (QEMU 'virt', AArch64).

  PcdFvBaseAddress/PcdFvSize are patched at build time by GenFds, straight
  from the FD region line that places FVMAIN_COMPACT
  (ArmVirtPkg/ArmVirtQemu.fdf: "0x00001000|$(FVMAIN_COMPACT_SIZE)
  gArmTokenSpaceGuid.PcdFvBaseAddress|gArmTokenSpaceGuid.PcdFvSize FV =
  FVMAIN_COMPACT"). SEC itself (ArmPlatformPkg/Sec/Sec.c) reads the same
  pair for the same reason: to know where its own flash-resident firmware
  is without guessing at runtime.
**/

#include <Uefi.h>
#include <Library/PlatformRomInfoLib.h>
#include <Library/PcdLib.h>

EFI_STATUS
EFIAPI
GetPlatformRomInfo (
  OUT EFI_PHYSICAL_ADDRESS  *RomBase,
  OUT UINT64                *RomSize
  )
{
  *RomBase = (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdFvBaseAddress);
  *RomSize = (UINT64)PcdGet32 (PcdFvSize);
  return EFI_SUCCESS;
}
