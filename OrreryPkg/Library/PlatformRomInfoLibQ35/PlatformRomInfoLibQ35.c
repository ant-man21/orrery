/** @file
  PlatformRomInfoLib instance for Q35 (OVMF/x86).

  PcdBfvBase/PcdBfvRawDataSize are OVMF's own "Boot Firmware Volume" PCDs —
  the base/size of the flash-resident CODE bank (FVMAIN_COMPACT + SECFV
  combined), patched at build time from OvmfPkgDefines.fdf.inc's
  CODE_BASE_ADDRESS/CODE_SIZE. OVMF's own TDX/SEV measurement code
  (PeilessStartupLib) reads the same pair for the same reason: to know
  where its own flash-resident firmware is without guessing at runtime.
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
  *RomBase = (EFI_PHYSICAL_ADDRESS)PcdGet32 (PcdBfvBase);
  *RomSize = (UINT64)PcdGet32 (PcdBfvRawDataSize);
  return EFI_SUCCESS;
}
