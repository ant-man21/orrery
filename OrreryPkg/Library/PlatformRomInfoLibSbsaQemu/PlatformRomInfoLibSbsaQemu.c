/** @file
  PlatformRomInfoLib instance for QEMU sbsa-ref (AArch64).

  PcdFvBaseAddress/PcdFvSize are patched at build time by GenFds, straight
  from the FD region line that places FVMAIN_COMPACT (SbsaQemu.fdf:
  "0x00000000|0x00300000 gArmTokenSpaceGuid.PcdFvBaseAddress|
  gArmTokenSpaceGuid.PcdFvSize FV = FVMAIN_COMPACT" inside [FD.SBSA_FLASH1]).
  Same pattern as PlatformRomInfoLibArmVirt — see docs/rom_discovery_story.md.
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
