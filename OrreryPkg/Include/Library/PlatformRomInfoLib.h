/** @file
  Platform abstraction for "where is my own boot firmware volume" — the
  flash-resident FV containing the running firmware code, which
  TpmProvisionApp/TpmVerifyBootApp measure into PCR[15] as "the ROM".

  Runtime discovery of this region was tried and abandoned twice (FV2/FVB2
  protocol enumeration, then GCD memory type) — see
  docs/rom_discovery_story.md for the full trail. Neither reliably
  identifies real flash content on either platform: DXE Core's generic
  FwVolBlock driver hands out the exact same FVB2 + GetPhysicalAddress
  combination for RAM-resident decompressed FVs as for real flash, and real
  flash content doesn't consistently get registered in the GCD memory space
  map either. Every platform already has this answer at build time, in a
  PCD its own FDF uses to lay out the FD — that's the authoritative source.

  One header, one function contract; each platform provides its own library
  instance reading that platform's own PCD pair, selected in the platform
  DSC's [LibraryClasses] section. Porting to new hardware (e.g. Tegra) means
  writing a new instance of this library, not touching the app.
**/

#ifndef PLATFORM_ROM_INFO_LIB_H_
#define PLATFORM_ROM_INFO_LIB_H_

#include <Uefi.h>

/**
  Returns the physical base address and size of this platform's boot
  firmware volume — the flash-resident region containing the running
  firmware code.

  @param[out]  RomBase  Physical base address of the boot firmware volume.
  @param[out]  RomSize  Size in bytes of the boot firmware volume.

  @retval EFI_SUCCESS   RomBase/RomSize populated.
**/
EFI_STATUS
EFIAPI
GetPlatformRomInfo (
  OUT EFI_PHYSICAL_ADDRESS  *RomBase,
  OUT UINT64                *RomSize
  );

#endif
