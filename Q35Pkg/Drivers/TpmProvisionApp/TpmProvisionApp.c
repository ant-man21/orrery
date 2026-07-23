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

#include <Pi/PiFirmwareFile.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/Tcg2Protocol.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/FirmwareVolumeBlock.h>

#include <Library/Tpm2CommandLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/Tpm2PolicyPcrLib.h>
#include <Library/Tpm2PcrLib.h>
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

/* ── helper: read current ROM from flash ───────────────────────────────── *
 * Locates flash-resident firmware volumes via EFI_FIRMWARE_VOLUME2_PROTOCOL
 * instead of hardcoding OVMF's fixed 0xFFC00000 base — real hardware won't
 * have a static, known-in-advance flash address (issue #7).
 *
 * Not every FV2 handle is backed by actual flash: a volume decompressed
 * into RAM (e.g. PEIFV/DXEFV, unpacked from FVMAIN_COMPACT) also shows up
 * here but has no EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL on its handle, since
 * there's no physical block device behind it. Only volumes with both
 * protocols are real flash content, which is what we want to measure — a
 * decompressed RAM copy isn't "the ROM."
 *
 * This also naturally excludes the NV variable store: it's raw NVRAM (no
 * FV header, no file system), so it never shows up as an FV2 handle at
 * all, unlike the old hardcoded 4MB read, which pulled in the mutable
 * variable store alongside the actual firmware code.
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
    Print (L"[PROVISION] No firmware volumes found: %r\n", Status);
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  FvCount       = 0;
  TotalFvBytes  = 0;
  LowestAddress = MAX_UINT64;
  HighestEnd    = 0;

  for (Index = 0; Index < NumHandles; Index++) {
    EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *Fvb;
    EFI_PHYSICAL_ADDRESS                 FvAddress;
    EFI_FIRMWARE_VOLUME_HEADER           *FvHeader;

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareVolumeBlock2ProtocolGuid,
                    (VOID **)&Fvb
                    );
    if (EFI_ERROR (Status)) {
      continue;                 /* not flash-backed — skip */
    }

    Status = Fvb->GetPhysicalAddress (Fvb, &FvAddress);
    if (EFI_ERROR (Status)) {
      continue;
    }

    FvHeader = (EFI_FIRMWARE_VOLUME_HEADER *)(UINTN)FvAddress;
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
    Print (L"[PROVISION] No flash-backed firmware volumes found\n");
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
    Print (L"[PROVISION] Firmware volumes are not contiguous — refusing to read a merged ROM range\n");
    return EFI_UNSUPPORTED;
  }

  *RomSize   = (UINTN)(HighestEnd - LowestAddress);
  *RomBuffer = AllocateCopyPool (*RomSize, (VOID *)(UINTN)LowestAddress);
  if (*RomBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Print (
    L"[PROVISION] ROM snapshot: 0x%lx, %u bytes (%u firmware volume(s))\n",
    LowestAddress,
    (UINT32)*RomSize,
    (UINT32)FvCount
    );
  return EFI_SUCCESS;
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
    Print (L"[PROVISION] OpenPcrPolicySession (trial) failed: %r\n", Status);
    return Status;
  }

  BuildPcrSelection (PCR_FOR_BIOS, &Pcrs);
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
    Print (L"[PROVISION] OpenPcrPolicySession (real) failed: %r\n", Status);
    return Status;
  }

  BuildPcrSelection (PCR_FOR_BIOS, &Pcrs);
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
  {
    TPML_DIGEST  Digests;

    Status = ExtendPcr (Tcg2, PCR_FOR_BIOS, RomBuffer, RomSize, "BIOS ROM", &Digests);
    if (EFI_ERROR (Status)) {
      Print (L"[PROVISION] ExtendPcr failed: %r\n", Status);
      FreePool (RomBuffer);
      return Status;
    }

    if (Digests.count > 0) {
      UINT32  i;

      Print (L"[PROVISION] PCR[%u] = ", PCR_FOR_BIOS);
      for (i = 0; i < Digests.digests[0].size; i++) {
        Print (L"%02x", Digests.digests[0].buffer[i]);
      }

      Print (L"\n");
    }
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