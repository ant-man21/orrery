/**
 * TpmVerifyBootApp.c
 *
 * "Reboot / No Update" verification flow (companion to TpmProvisionApp):
 *   1. Measure ROM -> extend PCR[16]                     [TODO]
 *   2. Load sealed blob from disk (fs1:\data\)            [TODO]
 *   3. Unseal (TPM checks PCR[16] + integrity)            [TODO]
 *   4. If success -> boot continues                       [TODO]
 *   5. If fail -> halt (BIOS modified!)                    [TODO]
 *
 * This app always measures the live ROM — there is no golden/reference
 * image to fall back to, on real hardware or here. To exercise the
 * tamper-detection path, flash a *different* ROM and re-run: step 1's
 * measurement produces a different PCR[16], so step 3's unseal is
 * expected to fail. To confirm the mechanism still succeeds, flash the
 * original ROM back and re-run.
 *
 * Companion app: TpmProvisionApp (first-boot provisioning flow).
 *
 * Build: UEFI application (not a driver). Load it from the UEFI shell:
 *   Shell> fs1:\apps\TpmVerifyBootApp.efi
 */

#include <Uefi.h>
#include <Pi/PiDxeCis.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>

#include <Pi/PiFirmwareFile.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/Tcg2Protocol.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/FirmwareVolumeBlock.h>
#include <Guid/SystemNvDataGuid.h>

#include <Library/Tpm2CommandLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/Tpm2PolicyPcrLib.h>
#include <Library/Tpm2PcrLib.h>
#include <IndustryStandard/Tpm20.h>
#include <IndustryStandard/TpmPtp.h>

/* ── constants ─────────────────────────────────────────────────────────── */
#define PCR_FOR_BIOS  16
#define SECRET_LEN        5
#define SECRET_NV_INDEX   ((TPM_HANDLE)0x01500001)   /* owner-defined NV index range: 0x01000000-0x01FFFFFF */

EFI_TCG2_PROTOCOL  *Tcg2;

/* ── helper: read current ROM from flash ───────────────────────────────── *
 * Locates flash-resident firmware volumes via EFI_FIRMWARE_VOLUME2_PROTOCOL
 * instead of hardcoding OVMF's fixed 0xFFC00000 base — real hardware won't
 * have a static, known-in-advance flash address (issue #7).
 *
 * "Has EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL with a working GetPhysicalAddress"
 * is NOT a reliable "this is real flash" test — verified wrong on both
 * platforms (see docs/rom_discovery_story.md for the full trail). DXE Core's
 * generic FwVolBlock driver installs that exact same protocol combination
 * for any FV that was handed off via a HOB too, including a GUIDed section
 * decompressed straight into a pool allocation (e.g. ArmVirtQemu's FvMain,
 * or Q35's DXEFV) — there is nothing about FVB2 presence that implies actual
 * block-device/flash backing.
 *
 * What's actually authoritative: QemuFlashFvbServicesRuntimeDxe (Q35) and
 * VirtNorFlashDxe (ArmVirt) both register their real flash windows with
 * gDS->AddMemorySpace(EfiGcdMemoryTypeMemoryMappedIo, ...) — that's the GCD
 * (DXE Services / Global Coherency Domain) memory space map, a lower-level,
 * more complete map than gBS->GetMemoryMap() (which only covers "system
 * memory" and won't list pure MMIO at all — confirmed via the shell's
 * `memmap` command finding no entry for Q35's flash). A pool-allocated
 * decompressed FV, by contrast, always lives in EfiGcdMemoryTypeSystemMemory.
 * So: keep only FVs whose physical address's GCD type is
 * EfiGcdMemoryTypeMemoryMappedIo.
 *
 * The NV variable store is additionally excluded by FileSystemGuid
 * (gEfiSystemNvDataFvGuid) as a belt-and-suspenders check — its contents
 * change every boot and have no business being measured into PCR[16] as
 * "the ROM," independent of whether it happens to be GCD-typed as MMIO.
 */
STATIC EFI_STATUS
ReadRomImage (
  OUT VOID   **RomBuffer,
  OUT UINTN   *RomSize
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE              *Handles;
  UINTN                   NumHandles;
  UINTN                   Index;
  UINTN                   FvCount;
  UINT64                  TotalFvBytes;
  EFI_PHYSICAL_ADDRESS    LowestAddress;
  EFI_PHYSICAL_ADDRESS    HighestEnd;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumHandles,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (NumHandles == 0)) {
    Print (L"[VERIFY] No firmware volumes found: %r\n", Status);
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  FvCount       = 0;
  TotalFvBytes  = 0;
  LowestAddress = MAX_UINT64;
  HighestEnd    = 0;

  Print (L"[VERIFY] %u total EFI_FIRMWARE_VOLUME2_PROTOCOL handle(s)\n", (UINT32)NumHandles);

  for (Index = 0; Index < NumHandles; Index++) {
    EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *Fvb;
    EFI_PHYSICAL_ADDRESS                 FvAddress;
    EFI_FIRMWARE_VOLUME_HEADER           *FvHeader;
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR      GcdDescriptor;
    BOOLEAN                              IsMmio;
    BOOLEAN                              IsNvram;

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareVolumeBlock2ProtocolGuid,
                    (VOID **)&Fvb
                    );
    if (EFI_ERROR (Status)) {
      Print (L"[VERIFY]   handle %u: no FVB2 protocol — skipped\n", (UINT32)Index);
      continue;
    }

    Status = Fvb->GetPhysicalAddress (Fvb, &FvAddress);
    if (EFI_ERROR (Status)) {
      Print (L"[VERIFY]   handle %u: FVB2 GetPhysicalAddress failed: %r — skipped\n", (UINT32)Index, Status);
      continue;
    }

    FvHeader = (EFI_FIRMWARE_VOLUME_HEADER *)(UINTN)FvAddress;

    Status = gDS->GetMemorySpaceDescriptor (FvAddress, &GcdDescriptor);
    IsMmio  = !EFI_ERROR (Status) && (GcdDescriptor.GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo);
    IsNvram = CompareGuid (&FvHeader->FileSystemGuid, &gEfiSystemNvDataFvGuid);

    Print (
      L"[VERIFY]   handle %u: addr=0x%lx size=0x%lx guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x gcdType=%d%a%a\n",
      (UINT32)Index,
      FvAddress,
      FvHeader->FvLength,
      FvHeader->FileSystemGuid.Data1,
      FvHeader->FileSystemGuid.Data2,
      FvHeader->FileSystemGuid.Data3,
      FvHeader->FileSystemGuid.Data4[0],
      FvHeader->FileSystemGuid.Data4[1],
      FvHeader->FileSystemGuid.Data4[2],
      FvHeader->FileSystemGuid.Data4[3],
      FvHeader->FileSystemGuid.Data4[4],
      FvHeader->FileSystemGuid.Data4[5],
      FvHeader->FileSystemGuid.Data4[6],
      FvHeader->FileSystemGuid.Data4[7],
      EFI_ERROR (Status) ? -1 : (INT32)GcdDescriptor.GcdMemoryType,
      IsMmio ? " (MMIO)" : " (not MMIO, skipped)",
      IsNvram ? " (NVRAM, skipped)" : ""
      );

    if (!IsMmio || IsNvram) {
      continue;                 /* RAM copy or NV variable store — not "the ROM" */
    }

    FvCount++;
    TotalFvBytes += FvHeader->FvLength;
    if (FvAddress < LowestAddress) {
      LowestAddress = FvAddress;
    }

    if (FvAddress + FvHeader->FvLength > HighestEnd) {
      HighestEnd = FvAddress + FvHeader->FvLength;
    }
  }

  FreePool (Handles);

  if (FvCount == 0) {
    Print (L"[VERIFY] No flash-backed firmware volumes found\n");
    return EFI_NOT_FOUND;
  }

  /*
   * Q35Pkg's FDF packs the flash-resident FVs (FVMAIN_COMPACT + SECFV)
   * back-to-back with no gap, so the merged [lowest, highest) span should
   * exactly equal the sum of the individual FV sizes. If it doesn't,
   * something about the flash layout isn't what this code assumes —
   * refuse to guess, rather than silently reading whatever sits in a gap.
   */
  if (TotalFvBytes != (HighestEnd - LowestAddress)) {
    Print (L"[VERIFY] Firmware volumes are not contiguous — refusing to read a merged ROM range\n");
    return EFI_UNSUPPORTED;
  }

  *RomSize   = (UINTN)(HighestEnd - LowestAddress);
  *RomBuffer = AllocateCopyPool (*RomSize, (VOID *)(UINTN)LowestAddress);
  if (*RomBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Print (
    L"[VERIFY] ROM snapshot: 0x%lx, %u bytes (%u firmware volume(s))\n",
    LowestAddress,
    (UINT32)*RomSize,
    (UINT32)FvCount
    );
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
VerifyBoot (
  OUT TPM2B_MAX_BUFFER  *OutData
  )
{
  EFI_STATUS  Status;
  TPMI_SH_AUTH_SESSION  RealSession;
  TPML_PCR_SELECTION    Pcrs;
  TPM2B_DIGEST          EmptyPcrDigest;
  TPMS_AUTH_COMMAND     AuthSession;

  BuildPcrSelection (PCR_FOR_BIOS, &Pcrs);
  ZeroMem (&EmptyPcrDigest, sizeof (EmptyPcrDigest));
  ZeroMem (&AuthSession, sizeof (AuthSession));
  ZeroMem (OutData, sizeof (*OutData));

  Status = Tpm2RequestUseTpm ();
  if (EFI_ERROR (Status)) {
    Print (L"[VERIFY] Tpm2RequestUseTpm failed: %r\n", Status);
    return Status;
  }

  Status = OpenPcrPolicySession (TPM_SE_POLICY, &RealSession);
  if (EFI_ERROR (Status)) {
    Print (L"[VERIFY] OpenPcrPolicySession (real) failed: %r\n", Status);
    return Status;
  }
  AuthSession.sessionHandle = RealSession;

  Status = Tpm2PolicyPCR (RealSession, &EmptyPcrDigest, &Pcrs);
  if (EFI_ERROR (Status)) {
    Print (L"[VERIFY] Tpm2PolicyPCR (real) failed: %r\n", Status);
    Tpm2FlushContext (RealSession);
    return Status;
  }

  Status = Tpm2NvRead (SECRET_NV_INDEX, SECRET_NV_INDEX, &AuthSession, SECRET_LEN, 0, OutData);
  Tpm2FlushContext (RealSession);
  return Status;
}

/* ── EFI entry point ──────────────────────────────────────────────────── */
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS          Status;
  TPM2B_MAX_BUFFER    OutData;
  VOID               *RomBuffer = NULL;
  UINTN               RomSize   = 0;

  Print (L"\n=== TpmVerifyBootApp (Reboot / Verify) ===\n\n");
  DEBUG ((DEBUG_INFO, "TpmVerifyBootApp start...\n"));
  Status = gBS->LocateProtocol (&gEfiTcg2ProtocolGuid,NULL,(VOID **)&Tcg2);
  if (EFI_ERROR (Status)) {
    Print (L"[VERIFY] TCG2 protocol not found: %r\n", Status);
    return EFI_NOT_FOUND;
  }

  Status = ReadRomImage (&RomBuffer, &RomSize);
  if (EFI_ERROR (Status)) {
    Print (L"[VERIFY] ReadRomImage failed: %r\n", Status);
    return Status;
  }

  {
    TPML_DIGEST  Digests;

    Status = ExtendPcr (Tcg2, PCR_FOR_BIOS, RomBuffer, RomSize, "BIOS ROM", &Digests);
    FreePool (RomBuffer);
    if (EFI_ERROR (Status)) {
      Print (L"[VERIFY] ExtendPcr failed: %r\n", Status);
      return Status;
    }

    if (Digests.count > 0) {
      UINT32  i;

      Print (L"[VERIFY] PCR[%u] = ", PCR_FOR_BIOS);
      for (i = 0; i < Digests.digests[0].size; i++) {
        Print (L"%02x", Digests.digests[0].buffer[i]);
      }

      Print (L"\n");
    }
  }

  Status = VerifyBoot (&OutData);
  if (EFI_ERROR (Status)) {
    Print (L"[VERIFY] Unseal FAILED — BIOS modified, halting.\n");
    // ASSERT_EFI_ERROR (Status); //could assert here
    return Status;
  }

  Print (L"[VERIFY] Unseal succeeded — PCR[%u] matches sealed state, continuing boot.\n",
         PCR_FOR_BIOS);

  Print (L"[VERIFY] Secret: ");
  for (UINTN i = 0; i < OutData.size; i++) {
    Print (L"%c", (CHAR16)OutData.buffer[i]);
  }
  Print (L"\n");

  Print (L"\n=== TpmVerifyBootApp done (%r) ===\n\n", Status);
  return Status;
}
