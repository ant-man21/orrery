/**
 * TpmProvisionApp.c
 *
 * "Setup First Boot" provisioning flow:
 *   1. Flash good ROM (signed)                          [external — not this app]
 *   2. Boot                                              [done — this is entry]
 *   3. Measure ROM -> extend PCR[16]                     [done]
 *   4. Create secret                                     [TODO]
 *   5. Seal secret to PCR[16]                            [TODO]
 *   6. Store public key in TPM NVRAM (write-once)        [future]
 *
 * Steps 4-5 are the commented-out SealUnsealRoundTrip() below — next up.
 *
 * Dev/test aid (not part of the production flow): this app also snapshots
 * the running ROM to fs1:\data\dummy.fd. That "golden" ROM image lets
 * TpmVerifyBootApp re-measure the *original* ROM on demand, so the
 * tamper-detection path can be exercised without needing to actually
 * reflash firmware between test runs.
 *
 * Companion app: TpmVerifyBootApp (reboot / unseal / tamper-detect flow).
 *
 * Build: UEFI application (not a driver). Load it from the UEFI shell:
 *   Shell> fs1:\apps\TpmProvisionApp.efi
 */

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/Tpm2CommandLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>

#include <Protocol/Tcg2Protocol.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileSystemInfo.h>
// #include <Protocol/FirmwareVolume2.h>

#include <Library/Tpm2CommandLib.h>
#include <IndustryStandard/Tpm20.h>
#include <IndustryStandard/TpmPtp.h>
/* ── constants ─────────────────────────────────────────────────────────── */

#define PCR_FOR_BIOS      16          /* user-controlled, safe to extend    */
#define DUMMY_FD_PATH     L"\\data\\dummy.fd"
#define SHARED_VOLUME_LABEL  L"SHARED"   /* matches `mformat -v SHARED` in qemu.sh */
#define SECRET_STRING     "hello"
#define SECRET_LEN        5

/* TPM2B_* helpers -------------------------------------------------------- */
/* A sealed blob returned by Tpm2CreateSealed is split into two parts:      */
/*   outPublic  – the TPM2B_PUBLIC  describing the object                   */
/*   outPrivate – the TPM2B_PRIVATE (encrypted sensitive area)              */
/* Both must be presented to Tpm2Load before you can call Tpm2Unseal.       */

/* ── helper: locate TCG2 protocol ──────────────────────────────────────── */

STATIC EFI_TCG2_PROTOCOL *
FindTcg2Protocol (VOID)
{
  EFI_STATUS          Status;
  EFI_TCG2_PROTOCOL  *Tcg2;

  Status = gBS->LocateProtocol (
                  &gEfiTcg2ProtocolGuid,
                  NULL,
                  (VOID **)&Tcg2
                  );
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] TCG2 protocol not found: %r\n", Status);
    return NULL;
  }
  return Tcg2;
}

/* ── helper: print TCG2 capabilities ───────────────────────────────────── */

STATIC VOID
PrintTcg2Caps (EFI_TCG2_PROTOCOL *Tcg2)
{
  EFI_STATUS                        Status;
  EFI_TCG2_BOOT_SERVICE_CAPABILITY  Cap;

  Cap.Size = sizeof (Cap);
  Status   = Tcg2->GetCapability (Tcg2, &Cap);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] GetCapability failed: %r\n", Status);
    return;
  }

  Print (L"[PROVISION] TPM present         : %s\n",
         Cap.TPMPresentFlag ? L"YES" : L"NO");
  Print (L"[PROVISION] Active PCR banks    : 0x%08x\n",
         Cap.ActivePcrBanks);
  Print (L"[PROVISION] TPM2 supported      : %s\n",
         (Cap.ProtocolVersion.Major >= 1) ? L"YES" : L"NO");
}

/* ── helper: read current ROM from flash ───────────────────────────────── */
STATIC EFI_STATUS
ReadRomImage (
  OUT VOID   **RomBuffer,
  OUT UINTN   *RomSize
  )
{
  /*
   * OVMF maps its 4 MB flash at 0xFFC00000 on X64.
   * We just snapshot that range directly — no FV2 protocol needed.
   */
  VOID   *FlashBase = (VOID *)(UINTN)0xFFC00000;
  UINTN   FlashSize = SIZE_4MB;

  *RomBuffer = AllocateCopyPool (FlashSize, FlashBase);
  if (*RomBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  *RomSize = FlashSize;
  Print (L"[PROVISION] ROM snapshot: 0xFFC00000, %u bytes\n", (UINT32)FlashSize);
  return EFI_SUCCESS;
}

/* ── helper: extend PCR[16] with arbitrary data ─────────────────────────── */
STATIC EFI_STATUS
ExtendPcr16 (
  IN EFI_TCG2_PROTOCOL  *Tcg2,
  IN VOID               *Data,
  IN UINTN               DataSize
  )
{
  EFI_STATUS                 Status;
  EFI_TCG2_EVENT            *Event;
  UINTN                      EventSize;
  UINT32                     UpdateCounter;

  /*
   * HashLogExtendEvent expects a caller-allocated EFI_TCG2_EVENT header.
   * The event log entry type EV_POST_CODE is conventional for ROM data.
   */
  EventSize = sizeof (EFI_TCG2_EVENT) + sizeof ("BIOS ROM") - 1;
  Event     = AllocateZeroPool (EventSize);
  if (Event == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Event->Size                         = (UINT32)EventSize;
  Event->Header.HeaderSize            = sizeof (EFI_TCG2_EVENT_HEADER);
  Event->Header.HeaderVersion         = EFI_TCG2_EVENT_HEADER_VERSION;
  Event->Header.PCRIndex              = PCR_FOR_BIOS;
  Event->Header.EventType             = EV_POST_CODE;
  CopyMem (Event->Event, "BIOS ROM", sizeof ("BIOS ROM") - 1);
  DEBUG ((DEBUG_INFO, "ExtendPcr16 [1]\n"));

  Status = Tcg2->HashLogExtendEvent (
                   Tcg2,
                   0,                            /* Flags                  */
                   (EFI_PHYSICAL_ADDRESS)(UINTN)Data,
                   (UINT64)DataSize,
                   Event
                   );
  FreePool (Event);
  DEBUG ((DEBUG_INFO, "ExtendPcr16 [2]\n"));

  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] HashLogExtendEvent failed: %r\n", Status);
    return Status;
  }

  /* Print the resulting PCR value for verification */
  {
    TPML_DIGEST  Digests;
    TPMS_PCR_SELECTION PcrSel;

    ZeroMem (&PcrSel, sizeof (PcrSel));
    PcrSel.hash = TPM_ALG_SHA256;
    PcrSel.sizeofSelect = 3;
    PcrSel.pcrSelect[PCR_FOR_BIOS / 8] = (UINT8)(1 << (PCR_FOR_BIOS % 8));

    TPML_PCR_SELECTION In;
    TPML_PCR_SELECTION Out;
    ZeroMem (&In, sizeof (In));
    ZeroMem (&Out, sizeof (Out));
    In.count = 1;
    In.pcrSelections[0] = PcrSel;
    DEBUG ((DEBUG_INFO, "ExtendPcr16 [3]\n"));

    Status = Tpm2RequestUseTpm ();
    if (EFI_ERROR (Status)) {
      Print (L"[PROVISION] Tpm2RequestUseTpm failed: %r\n", Status);
      return Status;
    }

    Status = Tpm2PcrRead (&In, &UpdateCounter, &Out, &Digests);
    DEBUG ((DEBUG_INFO, "ExtendPcr16 [4]\n"));
    if (!EFI_ERROR (Status) && Digests.count > 0) {
      UINT32 i;
      Print (L"[PROVISION] PCR[%u] = ", PCR_FOR_BIOS);
      for (i = 0; i < Digests.digests[0].size; i++) {
        Print (L"%02x", Digests.digests[0].buffer[i]);
      }
      Print (L"\n");
    }
  }

  return EFI_SUCCESS;
}

/* ── helper: open the shared (fs1:) volume by label ─────────────────────── */
STATIC EFI_FILE_PROTOCOL *
OpenSharedVolume (VOID)
{
  EFI_STATUS                        Status;
  UINTN                              NumHandles;
  EFI_HANDLE                        *Handles;
  UINTN                              Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   *Fs;
  EFI_FILE_PROTOCOL                 *Root;
  EFI_FILE_PROTOCOL                 *Fallback;
  UINT8                              InfoBuf[512];
  UINTN                              InfoSize;
  EFI_FILE_SYSTEM_INFO              *Info;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &NumHandles,
                  &Handles
                  );
  if (EFI_ERROR (Status) || NumHandles == 0) {
    Print (L"[PROVISION] No filesystem found: %r\n", Status);
    return NULL;
  }

  Fallback = NULL;
  for (Index = 0; Index < NumHandles; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    InfoSize = sizeof (InfoBuf);
    Status   = Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &InfoSize, InfoBuf);
    if (!EFI_ERROR (Status)) {
      Info = (EFI_FILE_SYSTEM_INFO *)InfoBuf;
      if (StrCmp (Info->VolumeLabel, SHARED_VOLUME_LABEL) == 0) {
        if (Fallback != NULL) {
          Fallback->Close (Fallback);
        }
        FreePool (Handles);
        return Root;
      }
    }

    if (Fallback == NULL) {
      Fallback = Root;
    } else {
      Root->Close (Root);
    }
  }

  FreePool (Handles);
  if (Fallback != NULL) {
    Print (L"[PROVISION] Warning: no volume labeled %s found — falling back to first filesystem\n",
           SHARED_VOLUME_LABEL);
  }
  return Fallback;
}

/* ── helper: save buffer to a file on the shared (fs1:) volume ──────────── */

STATIC EFI_STATUS
SaveFileToDisk (
  IN CHAR16  *FileName,
  IN VOID    *Data,
  IN UINTN    DataSize
  )
{
  EFI_STATUS          Status;
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *File;
  UINTN               WriteSize;

  Root = OpenSharedVolume ();
  if (Root == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = Root->Open (
                   Root, &File, FileName,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   0
                   );
  if (EFI_ERROR (Status)) {
    Root->Close (Root);
    Print (L"[PROVISION] Cannot create %s: %r\n", FileName, Status);
    return Status;
  }

  WriteSize = DataSize;
  Status    = File->Write (File, &WriteSize, Data);
  File->Close (File);
  Root->Close (Root);

  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Write failed: %r\n", Status);
  } else {
    Print (L"[PROVISION] Saved %u bytes to %s\n", (UINT32)WriteSize, FileName);
  }
  return Status;
}

/* ── helper: seal / unseal round-trip ──────────────────────────────────── */
STATIC EFI_STATUS
SealUnsealRoundTrip (
  IN UINT8   *Secret,
  IN UINTN    SecretLen
  )
{
  EFI_STATUS          Status = EFI_SUCCESS;

  return Status;
}

/* ── EFI entry point ───────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS          Status;
  EFI_TCG2_PROTOCOL  *Tcg2;
  VOID               *RomBuffer = NULL;
  UINTN               RomSize   = 0;

  Print (L"\n=== TpmProvisionApp (Setup First Boot) ===\n\n");
  DEBUG ((DEBUG_INFO, "TpmProvisionApp start...\n"));

  /* Step 1: init TPM / locate TCG2 protocol, print caps */
  Tcg2 = FindTcg2Protocol ();
  if (Tcg2 == NULL) {
    return EFI_NOT_FOUND;
  }
  PrintTcg2Caps (Tcg2);
  Print (L"\n");

  /* Step 2 (boot) is implicit — we're running.                            */

  /* Step 3: Measure ROM -> extend PCR[16] */
  Print (L"[PROVISION] Reading ROM image...\n");
  Status = ReadRomImage (&RomBuffer, &RomSize);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] ReadRomImage failed: %r\n", Status);
    return Status;
  }

  Print (L"[PROVISION] Extending PCR[%u]...\n", PCR_FOR_BIOS);
  Status = ExtendPcr16 (Tcg2, RomBuffer, RomSize);
  if (EFI_ERROR (Status)) {
    FreePool (RomBuffer);
    return Status;
  }
  Print (L"\n");

  /*
   * Dev/test aid, not one of the 7 provisioning steps: snapshot the ROM
   * we just measured to fs1:\data\dummy.fd. TpmVerifyBootApp can re-hash
   * this "golden" copy on demand to simulate "the original ROM" without
   * needing a second physical firmware build.
   */
  Print (L"[PROVISION] Saving ROM snapshot to fs1:" DUMMY_FD_PATH L"...\n");
  Status = SaveFileToDisk (DUMMY_FD_PATH, RomBuffer, RomSize);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Warning: could not save dummy.fd (%r) – continuing\n",
           Status);
  }
  Print (L"\n");

  /* Steps 4-6: create secret, seal to PCR[16], store sealed blob on disk */
  Print (L"[PROVISION] Running seal→unseal round-trip with secret \""
         SECRET_STRING L"\"...\n");
  Status = SealUnsealRoundTrip (
             (UINT8 *)SECRET_STRING,
             SECRET_LEN
             );

  FreePool (RomBuffer);

  Print (L"\n=== TpmProvisionApp done (%r) ===\n\n", Status);
  return Status;
}
