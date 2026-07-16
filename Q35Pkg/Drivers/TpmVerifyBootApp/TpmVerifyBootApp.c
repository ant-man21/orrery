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
 * This is a skeleton: each step below is a stand-in that prints what it
 * will do and returns EFI_SUCCESS. Nothing here talks to the TPM yet.
 * Fill these in alongside TpmProvisionApp's seal step (its
 * SealUnsealRoundTrip()), since step 3 here is that same unseal logic run
 * against whatever PCR[16] happens to be *this* boot.
 *
 * Planned dev/test mode: after flashing a *different* ROM, step 1's
 * measurement produces a different PCR[16], so step 3's unseal is
 * expected to fail — that's the tamper-detection path working as
 * intended. To confirm the mechanism still succeeds against the
 * *original* image (without needing a second physical firmware build),
 * a future flag here should re-measure fs1:\data\dummy.fd — the golden
 * ROM snapshot TpmProvisionApp saves — instead of the live ROM.
 *
 * Companion app: TpmProvisionApp (first-boot provisioning flow).
 *
 * Build: UEFI application (not a driver). Load it from the UEFI shell:
 *   Shell> fs1:\apps\TpmVerifyBootApp.efi
 */

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>

#include <Protocol/Tcg2Protocol.h>

#define PCR_FOR_BIOS  16

/* ── Step 1: measure ROM -> extend PCR[16] ───────────────────────────────
 * TODO: this duplicates ReadRomImage()/ExtendPcr16() from
 * TpmProvisionApp.c. Once both apps do real work, factor the shared
 * measure-and-extend logic out instead of copy-pasting it here.
 */
STATIC EFI_STATUS
MeasureRomAndExtendPcr (VOID)
{
  Print (L"[VERIFY] Step 1: measure ROM -> extend PCR[%u]  (TODO)\n", PCR_FOR_BIOS);
  return EFI_SUCCESS;
}

/* ── Step 2: load sealed blob from disk ──────────────────────────────────
 * TODO: read the (OutPublic, OutPrivate) blob pair that TpmProvisionApp
 * will write to fs1:\data\ once its seal step lands.
 */
STATIC EFI_STATUS
LoadSealedBlob (VOID)
{
  Print (L"[VERIFY] Step 2: load sealed blob from fs1:\\data\\  (TODO)\n");
  return EFI_SUCCESS;
}

/* ── Step 3: unseal against current PCR[16] ──────────────────────────────
 * TODO: Tpm2Load + PolicyPCR session + Tpm2Unseal, mirroring the
 * commented-out SealUnsealRoundTrip() in TpmProvisionApp.c.
 */
STATIC EFI_STATUS
UnsealSecret (VOID)
{
  Print (L"[VERIFY] Step 3: unseal secret against PCR[%u]  (TODO)\n", PCR_FOR_BIOS);
  return EFI_SUCCESS;
}

/* ── EFI entry point ──────────────────────────────────────────────────── */

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS  Status;

  Print (L"\n=== TpmVerifyBootApp (Reboot / Verify) ===\n\n");
  DEBUG ((DEBUG_INFO, "TpmVerifyBootApp start...\n"));

  Status = MeasureRomAndExtendPcr ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = LoadSealedBlob ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UnsealSecret ();
  if (EFI_ERROR (Status)) {
    /* Step 5: fail -> halt (BIOS modified!) */
    Print (L"[VERIFY] Unseal FAILED — BIOS modified, halting.\n");
    return Status;
  }

  /* Step 4: success -> boot continues */
  Print (L"[VERIFY] Unseal succeeded — PCR[%u] matches golden state, continuing boot.\n",
         PCR_FOR_BIOS);

  Print (L"\n=== TpmVerifyBootApp done (%r) ===\n\n", Status);
  return Status;
}
