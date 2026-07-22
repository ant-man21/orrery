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
#include <Library/Tpm2PcrLib.h>
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
