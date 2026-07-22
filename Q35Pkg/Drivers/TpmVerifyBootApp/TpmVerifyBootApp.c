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
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>

#include <Protocol/Tcg2Protocol.h>

#include <Library/Tpm2CommandLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/Tpm2PolicyPcrLib.h>
#include <IndustryStandard/Tpm20.h>
#include <IndustryStandard/TpmPtp.h>

/* ── constants ─────────────────────────────────────────────────────────── */
#define PCR_FOR_BIOS  16
#define SECRET_LEN        5
#define SECRET_NV_INDEX   ((TPM_HANDLE)0x01500001)   /* owner-defined NV index range: 0x01000000-0x01FFFFFF */

EFI_TCG2_PROTOCOL  *Tcg2;

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
  VOID   *FlashBase = (VOID *)(UINTN)0xFFC00000; // TODO for real hardware, use FV2 protocol to locate the flash volume instead of hardcoding this address
  UINTN   FlashSize = SIZE_4MB;

  *RomBuffer = AllocateCopyPool (FlashSize, FlashBase);
  if (*RomBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  *RomSize = FlashSize;
  Print (L"[VERIFY] ROM snapshot: 0xFFC00000, %u bytes\n", (UINT32)FlashSize);
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

  Status = Tcg2->HashLogExtendEvent (
                   Tcg2,
                   0,                            /* Flags                  */
                   (EFI_PHYSICAL_ADDRESS)(UINTN)Data,
                   (UINT64)DataSize,
                   Event
                   );
  FreePool (Event);

  if (EFI_ERROR (Status)) {
    Print (L"[VERIFY] HashLogExtendEvent failed: %r\n", Status);
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

    Status = Tpm2RequestUseTpm ();
    if (EFI_ERROR (Status)) {
      Print (L"[VERIFY] Tpm2RequestUseTpm failed: %r\n", Status);
      return Status;
    }

    Status = Tpm2PcrRead (&In, &UpdateCounter, &Out, &Digests);
    if (!EFI_ERROR (Status) && Digests.count > 0) {
      UINT32 i;
      Print (L"[VERIFY] PCR[%u] = ", PCR_FOR_BIOS);
      for (i = 0; i < Digests.digests[0].size; i++) {
        Print (L"%02x", Digests.digests[0].buffer[i]);
      }
      Print (L"\n");
    }
  }

  return EFI_SUCCESS;
}

/* ── helper: open a session (trial or real policy) ───────────────────────
 * Unbound, unsalted, no parameter encryption — we don't need any of that,
 * just a handle to fold TPM2_PolicyPCR assertions into.
 */
STATIC EFI_STATUS
OpenPcrPolicySession (
  IN  TPM_SE                 SessionType,
  OUT TPMI_SH_AUTH_SESSION  *SessionHandle
  )
{
  EFI_STATUS              Status;
  TPM2B_NONCE             NonceCaller;
  TPM2B_NONCE             NonceTpm;
  TPM2B_ENCRYPTED_SECRET  Salt;
  TPMT_SYM_DEF            Symmetric;

  ZeroMem (&NonceCaller, sizeof (NonceCaller));
  NonceCaller.size = SHA256_DIGEST_SIZE;

  ZeroMem (&Salt, sizeof (Salt));
  ZeroMem (&NonceTpm, sizeof (NonceTpm));
  Symmetric.algorithm = TPM_ALG_NULL;

  Status = Tpm2StartAuthSession (
             TPM_RH_NULL,               /* TpmKey  – unsalted            */
             TPM_RH_NULL,               /* Bind    – unbound             */
             &NonceCaller,
             &Salt,
             SessionType,
             &Symmetric,
             TPM_ALG_SHA256,
             SessionHandle,
             &NonceTpm
             );
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2StartAuthSession failed: %r\n", Status);
  }

  return Status;
}

STATIC VOID
BuildPcr16Selection (
  OUT TPML_PCR_SELECTION  *Pcrs
  )
{
  ZeroMem (Pcrs, sizeof (*Pcrs));
  Pcrs->count                       = 1;
  Pcrs->pcrSelections[0].hash         = TPM_ALG_SHA256;
  Pcrs->pcrSelections[0].sizeofSelect = 3;
  Pcrs->pcrSelections[0].pcrSelect[PCR_FOR_BIOS / 8] = (UINT8)(1 << (PCR_FOR_BIOS % 8));
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

  BuildPcr16Selection (&Pcrs);
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
  Status = ExtendPcr16 (Tcg2, RomBuffer, RomSize);

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
