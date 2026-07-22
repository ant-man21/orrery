/**
 * TpmProvisionApp.c
 *
 * "Setup First Boot" provisioning flow:
 *   1. Flash good ROM (signed)                            [external — not this app]
 *   2. Boot                                               [done — this is entry]
 *   3. Measure ROM -> extend PCR[16]                      [done]
 *   4. Compute PCR[16] policy digest (trial session)      [done]
 *   5. Define an NV index gated on that policy            [done]
 *   6. Write the secret into it via a real policy session [done]
 *   7. Store public key in TPM NVRAM (write-once)         [future]
 *
 * Steps 4-6 are ProvisionSecretInNvram() below.
 *
 * Demo flow: this app runs once, against a wiped/clean TPM, to define the
 * NV index and seal the secret to PCR[16]. Every subsequent boot runs
 * TpmVerifyBootApp instead, which measures the live ROM only — there is
 * no golden/reference image on real hardware, so none is used here either.
 *
 * Open question: re-running this app against an already-provisioned TPM
 * (e.g. after a legitimate firmware update changes PCR[16]) currently
 * fails the NV write step silently, since the index is still gated on the
 * old policy. See issue #5 (OTA research) for the re-provisioning story.
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
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>

#include <Protocol/Tcg2Protocol.h>

#include <Library/Tpm2CommandLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/Tpm2PolicyPcrLib.h>
#include <IndustryStandard/Tpm20.h>
#include <IndustryStandard/TpmPtp.h>
/* ── constants ─────────────────────────────────────────────────────────── */

#define PCR_FOR_BIOS      16                   // user-controlled, safe to extend
#define SECRET_STRING     "hello"
#define SECRET_LEN        5
#define SECRET_NV_INDEX   ((TPM_HANDLE)0x01500001)   /* owner-defined NV index range: 0x01000000-0x01FFFFFF */
EFI_TCG2_PROTOCOL  *Tcg2;

/* ── helper: print TCG2 capabilities ───────────────────────────────────── */
STATIC VOID
PrintTcg2Caps (EFI_TCG2_PROTOCOL *Tcg2)
{
  EFI_STATUS                        Status;
  EFI_TCG2_BOOT_SERVICE_CAPABILITY  Capability;

  Capability.Size = sizeof (Capability);
  Status   = Tcg2->GetCapability (Tcg2, & Capability);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] GetCapability failed: %r\n", Status);
    return;
  }

  Print (L"[PROVISION] TPM present         : %s\n", Capability.TPMPresentFlag ? L"YES" : L"NO");
  Print (L"[PROVISION] Active PCR banks    : 0x%08x\n", Capability.ActivePcrBanks);
  Print (L"[PROVISION] TPM2 supported      : %s\n", (Capability.ProtocolVersion.Major >= 1) ? L"YES" : L"NO");
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
  VOID   *FlashBase = (VOID *)(UINTN)0xFFC00000; // TODO for real hardware, use FV2 protocol to locate the flash volume instead of hardcoding this address
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

/* ── helper: PCR selection for PCR[16], SHA256 bank ──────────────────────
 * TPML_PCR_SELECTION with a single bank entry selecting just PCR_FOR_BIOS.
 * sizeofSelect = 3 covers PCRs 0-23; pcrSelect is a bitmask, one bit per PCR.
 */
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

/* ── helper: compute the PCR[16] policy digest via a trial session ───────
 * This is the "what value should lock the NV index" leg — nothing is
 * actually authorized here, we're just asking the TPM to precompute the
 * digest that a real session satisfying the same assertion would produce.
 */
STATIC EFI_STATUS
ComputePcr16PolicyDigest (
  OUT TPM2B_DIGEST  *PolicyDigest
  )
{
  EFI_STATUS            Status;
  TPMI_SH_AUTH_SESSION  TrialSession;
  TPML_PCR_SELECTION    Pcrs;
  TPM2B_DIGEST          EmptyPcrDigest;

  Status = OpenPcrPolicySession (TPM_SE_TRIAL, &TrialSession);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  BuildPcr16Selection (&Pcrs);
  ZeroMem (&EmptyPcrDigest, sizeof (EmptyPcrDigest));   /* size=0 -> TPM uses live PCR value */

  Status = Tpm2PolicyPCR (TrialSession, &EmptyPcrDigest, &Pcrs);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2PolicyPCR (trial) failed: %r\n", Status);
    Tpm2FlushContext (TrialSession);
    return Status;
  }

  Status = Tpm2PolicyGetDigest (TrialSession, PolicyDigest);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2PolicyGetDigest failed: %r\n", Status);
  }

  Tpm2FlushContext (TrialSession);
  return Status;
}

/* ── helper: define the secret's NV index, gated on the PCR[16] policy ──
 * POLICYWRITE|POLICYREAD (not AUTHWRITE/AUTHREAD) means the policy is the
 * only door — there is no password fallback into this index.
 */
STATIC EFI_STATUS
DefineSecretNvIndex (
  IN TPM2B_DIGEST  *PolicyDigest
  )
{
  EFI_STATUS       Status;
  TPM2B_NV_PUBLIC  NvPublic;
  TPM2B_AUTH       IndexAuth;

  ZeroMem (&NvPublic, sizeof (NvPublic));
  NvPublic.nvPublic.nvIndex   = SECRET_NV_INDEX;
  NvPublic.nvPublic.nameAlg   = TPM_ALG_SHA256;
  NvPublic.nvPublic.attributes.TPMA_NV_POLICYWRITE = 1;
  NvPublic.nvPublic.attributes.TPMA_NV_POLICYREAD  = 1;
  NvPublic.nvPublic.authPolicy.size = PolicyDigest->size;
  CopyMem (NvPublic.nvPublic.authPolicy.buffer, PolicyDigest->buffer, PolicyDigest->size);
  NvPublic.nvPublic.dataSize  = SECRET_LEN;
  /* NvPublic.size is the marshaled wire length of TPMS_NV_PUBLIC, not
   * sizeof(TPMS_NV_PUBLIC) — the C struct's authPolicy field reserves a
   * fixed 64-byte buffer (room for SHA-512) regardless of the hash
   * algorithm actually in use, but only PolicyDigest->size bytes of it
   * are ever written onto the wire.
   */
  NvPublic.size = (UINT16)(
                    sizeof (TPMI_RH_NV_INDEX) +   /* nvIndex             */
                    sizeof (TPMI_ALG_HASH)    +   /* nameAlg             */
                    sizeof (TPMA_NV)          +   /* attributes          */
                    sizeof (UINT16)           +   /* authPolicy.size     */
                    PolicyDigest->size        +   /* authPolicy.buffer   */
                    sizeof (UINT16)                /* dataSize           */
                    );

  ZeroMem (&IndexAuth, sizeof (IndexAuth));   /* no index password — policy only */

  Status = Tpm2NvDefineSpace (TPM_RH_OWNER, NULL, &IndexAuth, &NvPublic);
  if (Status == EFI_ALREADY_STARTED) {
    Print (L"[PROVISION] NV index 0x%x already defined — reusing it\n", SECRET_NV_INDEX);
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2NvDefineSpace failed: %r\n", Status);
  }

  return Status;
}

/* ── helper: prove PCR[16] live, then write the secret into the index ───
 * The real (non-trial) leg: same assertion as the trial run, but this
 * time it's actually checked against the TPM's current PCR[16].
 */
STATIC EFI_STATUS
WriteSecretToNvIndex (
  IN UINT8  *Secret,
  IN UINTN   SecretLen
  )
{
  EFI_STATUS            Status;
  TPMI_SH_AUTH_SESSION  RealSession;
  TPML_PCR_SELECTION    Pcrs;
  TPM2B_DIGEST          EmptyPcrDigest;
  TPMS_AUTH_COMMAND     AuthSession;
  TPM2B_MAX_BUFFER       InData;

  Status = OpenPcrPolicySession (TPM_SE_POLICY, &RealSession);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  BuildPcr16Selection (&Pcrs);
  ZeroMem (&EmptyPcrDigest, sizeof (EmptyPcrDigest));

  Status = Tpm2PolicyPCR (RealSession, &EmptyPcrDigest, &Pcrs);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2PolicyPCR (real) failed: %r\n", Status);
    Tpm2FlushContext (RealSession);
    return Status;
  }

  ZeroMem (&AuthSession, sizeof (AuthSession));
  AuthSession.sessionHandle = RealSession;   /* nonce/hmac left empty — no password on this index */

  ZeroMem (&InData, sizeof (InData));
  InData.size = (UINT16)SecretLen;
  CopyMem (InData.buffer, Secret, SecretLen);

  Status = Tpm2NvWrite (SECRET_NV_INDEX, SECRET_NV_INDEX, &AuthSession, &InData, 0);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2NvWrite failed: %r\n", Status);
  } else {
    Print (L"[PROVISION] Secret written to NV index 0x%x, gated on PCR[%u]\n", SECRET_NV_INDEX, PCR_FOR_BIOS);
  }

  Tpm2FlushContext (RealSession);
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
  VOID               *RomBuffer = NULL;
  UINTN               RomSize   = 0;
  TPM2B_DIGEST  PolicyDigest;

  Print (L"\n=== TpmProvisionApp (Setup First Boot) ===\n\n");
  DEBUG ((DEBUG_INFO, "TpmProvisionApp start...\n"));

  /* Step 1: init TPM / locate TCG2 protocol, print caps */
  Status = gBS->LocateProtocol (&gEfiTcg2ProtocolGuid,NULL,(VOID **)&Tcg2);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] TCG2 protocol not found: %r\n", Status);
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

  /* Steps 4-6: compute PCR[16] policy, define NV index, write secret into it */
  Print (L"[PROVISION] Sealing secret \"" SECRET_STRING L"\" to PCR[%u] via NV index 0x%x...\n",
         PCR_FOR_BIOS, SECRET_NV_INDEX);



  Status = Tpm2RequestUseTpm ();
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2RequestUseTpm failed: %r\n", Status);
    return Status;
  }

  Status = ComputePcr16PolicyDigest (&PolicyDigest);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = DefineSecretNvIndex (&PolicyDigest);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = WriteSecretToNvIndex ((UINT8 *)SECRET_STRING, SECRET_LEN); //TODO make this not hardcoded
  FreePool (RomBuffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Print (L"\n=== TpmProvisionApp done (%r) ===\n\n", Status);
  return Status;
}