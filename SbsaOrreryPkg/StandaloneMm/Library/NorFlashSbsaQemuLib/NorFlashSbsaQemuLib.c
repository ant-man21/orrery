/** @file

  NorFlashPlatformLib instance for QEMU sbsa-ref's secure variable store.

  Secure world's view of SBSA_FLASH0 (see NorFlashSbsaQemuLib.inf header):

  +--------------------------------+ 0x010C0000  (QEMU_SECURE_VARSTORE_BASE
  |     FTW Spare Store (256KB)    |              + QEMU_SECURE_VARSTORE_SIZE
  +--------------------------------+ 0x01080000   is 0x01100000; we only use
  |    FTW Working Store (256KB)   |              the first 768KB of the 1MB
  +--------------------------------+ 0x01040000   TF-A/QEMU reserve for it.)
  |    Variable Store (256KB)      |
  +--------------------------------+ 0x01000000
                                      (QEMU_SECURE_VARSTORE_BASE)

  Each region is a full 256KB to match the real erase-sector granularity
  QEMU's sbsa-ref secure pflash enforces — a smaller (64KB) per-region
  layout let FaultTolerantWriteStandaloneMm's workspace "reclaim" path
  erase across region boundaries and wipe out the Variable store's header.
  See SbsaOrreryStandaloneMm.dsc's NV Storage PCD comment for the story.

  Copyright (c) 2026, Orrery Project.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <Library/NorFlashPlatformLib.h>

//
// QEMU_SECURE_VARSTORE_BASE from trusted-firmware-a's
// plat/qemu/qemu_sbsa/include/platform_def.h. Must stay in sync with that
// header and with the PCDs in SbsaOrreryStandaloneMm.dsc / .fdf.
//
#define SBSA_SECURE_VARSTORE_BASE  0x01000000

//
// 3 regions (Variable/FtwWorking/FtwSpare) x 256KB each — see the file
// header comment for why each region needs the full 256KB.
//
#define SBSA_SECURE_VARSTORE_DEVICE_SIZE  (3 * SIZE_256KB)

NOR_FLASH_DESCRIPTION  mNorFlashDevices[] = {
  {
    SBSA_SECURE_VARSTORE_BASE,
    SBSA_SECURE_VARSTORE_BASE,
    SBSA_SECURE_VARSTORE_DEVICE_SIZE,
    SIZE_256KB,
  },
};

UINT32  mNorFlashCount = ARRAY_SIZE (mNorFlashDevices);

EFI_STATUS
NorFlashPlatformInitialization (
  VOID
  )
{
  //
  // Unlike real Versatile Express hardware, QEMU's secure pflash device
  // needs no write-protect register poke — it's writable as soon as it's
  // mapped in.
  //
  return EFI_SUCCESS;
}

EFI_STATUS
NorFlashPlatformGetDevices (
  OUT NOR_FLASH_DESCRIPTION  **NorFlashDevices,
  OUT UINT32                 *Count
  )
{
  if ((NorFlashDevices == NULL) || (Count == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *NorFlashDevices = mNorFlashDevices;
  *Count           = mNorFlashCount;

  return EFI_SUCCESS;
}
