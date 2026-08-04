/**
 * TpmProvisionApp.c
 *
 * "Setup First Boot" provisioning flow (issue #15 — PolicyAuthorize
 * signed-ticket reprovisioning, replacing the old PCR16-only design):
 *   1. Flash good ROM (signed)                              [external — not this app]
 *   2. Boot                                                 [done — this is entry]
 *   3. Measure ROM -> extend PCR[15]                        [done]
 *   4. Set a real TPM_RH_OWNER auth (was NULL — issue #15)  [done]
 *   5. Compute the PolicyAuthorize digest (trial session)   [done]
 *   6. Define an NV index gated on that digest              [done]
 *   7. Prove this boot's ROM against its ticket, then write
 *      a random secret via the resulting real session       [done]
 *
 * The vault's rule (step 6's authPolicy) is keyed to a compiled-in
 * signing key's Name (TrustedUpdateKey.h) — not to any PCR value — so it
 * never has to change again. Every future firmware update just ships a
 * new signed ticket for its own PCR15 value; this app and
 * TpmVerifyBootApp both run the same "prove this boot's ROM, then use
 * the resulting session" chain (RunAuthorizedPolicySession below) to
 * satisfy that fixed rule. See issue #15 for the full design writeup and
 * threat model.
 *
 * Demo flow: this app runs once, against a wiped/clean TPM, to define the
 * NV index and seal the secret. Every subsequent boot runs
 * TpmVerifyBootApp instead, which measures the live ROM only — there is
 * no golden/reference image on real hardware, so none is used here either.
 *
 * Companion app: TpmVerifyBootApp (reboot / unseal / tamper-detect flow) —
 * shares this same authorization chain, swapping the final Tpm2NvWrite
 * for a Tpm2NvRead.
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
#include <Library/RngLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/PlatformRomInfoLib.h>

#include <Protocol/Tcg2Protocol.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>

#include <Library/Tpm2CommandLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/Tpm2PolicyPcrLib.h>
#include <Library/Tpm2PcrLib.h>
#include <Library/Tpm2PolicyAuthorizeLib.h>
#include <IndustryStandard/Tpm20.h>
#include <IndustryStandard/TpmPtp.h>

#include <TrustedUpdateKey.h>

/* ── constants ─────────────────────────────────────────────────────────── */

/* PCR index lives in gOrreryPkgTokenSpaceGuid.PcdPcrForBios (OrreryPkg.dec)
 * — non-resettable (platform-reset only); outside the PC Client PFP's
 * PCR0-7 SRTM range to avoid firmware-phase collisions — see issue #27.
 */
#define SECRET_LEN        5
#define SECRET_NV_INDEX   ((TPM_HANDLE)0x01500001)   /* owner-defined NV index range: 0x01000000-0x01FFFFFF */

#define SHARED_VOLUME_LABEL  L"SHARED"
#define UPDATE_TICKET_PATH   L"\\data\\rom.ticket"

/*
 * Compiled-constant TPM_RH_OWNER auth — closes the "anyone can act as
 * Owner for free" hole from the previous NULL/empty auth (issue #15):
 * without this, anything on the box could undefine/redefine the NV index
 * under its own rule, bypassing PolicyAuthorize entirely.
 *
 * Caveat, carried forward deliberately rather than solved here: this
 * password is only as secret as the ROM itself — anyone who can dump
 * flash can read it too. It still forecloses the *free, no-effort*
 * empty-auth attack. Real defense against a stolen-owner-auth
 * redefine-and-fake-the-secret attack is GenerateRandomSecret() below —
 * a redefined index still can't produce a secret worth having, since the
 * real one is never written down anywhere an attacker can read it. A
 * stronger fix (TPM_RH_PLATFORM + TPMA_NV_POLICY_DELETE, making the
 * index literally undeletable) is a separate, riskier change — deferred.
 */
#define OWNER_AUTH_STRING  "orrery-owner-auth-v1"

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
 * Two rounds of runtime discovery (EFI_FIRMWARE_VOLUME2_PROTOCOL/FVB2
 * enumeration, then GCD memory type) both turned out to be unreliable on
 * both platforms — neither ever proved able to actually distinguish real
 * flash content from a RAM-resident decompressed FV. Full trail in
 * docs/rom_discovery_story.md.
 *
 * What every platform already has, correctly, at build time: a PCD its own
 * FDF uses to lay out the FD in the first place. PlatformRomInfoLib reads
 * that PCD — a different one per platform (OVMF's PcdBfvBase/
 * PcdBfvRawDataSize on Q35, ArmPkg's PcdFvBaseAddress/PcdFvSize on
 * ArmVirt), selected via the platform DSC's [LibraryClasses] section, not
 * a hardcoded address in this shared source (issue #7). Porting to new
 * hardware means writing a new PlatformRomInfoLib instance, not touching
 * this app.
 */
STATIC EFI_STATUS
ReadRomImage (
  OUT VOID   **RomBuffer,
  OUT UINTN   *RomSize
  )
{
  EFI_STATUS             Status;
  EFI_PHYSICAL_ADDRESS   RomBase;
  UINT64                 RomSize64;

  Status = GetPlatformRomInfo (&RomBase, &RomSize64);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] GetPlatformRomInfo failed: %r\n", Status);
    return Status;
  }

  /*
   * Printed before any attempt to read from RomBase, and before the sanity
   * checks below reject anything — so a wrong PCD resolution shows up here
   * as a visibly wrong number instead of a crash reading garbage memory.
   * This is a build-time PCD now (see docs/rom_discovery_story.md), not
   * runtime discovery, but it's a new mechanism on its first real test —
   * don't skip eyeballing this line against a known-good address/size.
   */
  Print (L"[PROVISION] PlatformRomInfoLib: base=0x%lx size=0x%lx (%lu KB)\n", RomBase, RomSize64, RomSize64 / 1024);

  if ((RomBase == 0) || (RomSize64 == 0)) {
    Print (L"[PROVISION] PlatformRomInfoLib returned an empty ROM region — refusing to read\n");
    return EFI_NOT_FOUND;
  }

  if (RomSize64 > SIZE_64MB) {
    Print (L"[PROVISION] PlatformRomInfoLib returned an implausibly large size — refusing to read\n");
    return EFI_UNSUPPORTED;
  }

  *RomSize   = (UINTN)RomSize64;
  *RomBuffer = AllocateCopyPool (*RomSize, (VOID *)(UINTN)RomBase);
  if (*RomBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Print (L"[PROVISION] ROM snapshot: 0x%lx, %u bytes\n", RomBase, (UINT32)*RomSize);
  return EFI_SUCCESS;
}

/* ── helper: locate the shared FAT volume by label ───────────────────────
 * fs0:/fs1: enumeration order is not stable across boots or handles, so
 * identify the shared volume by its FAT label ("SHARED", matching
 * qemu.sh's `mformat -v SHARED`) rather than assuming a fixed handle
 * index — same caution already documented for this project's file I/O.
 */
STATIC EFI_STATUS
LocateSharedVolume (
  OUT EFI_FILE_PROTOCOL  **Root
  )
{
  EFI_STATUS                        Status;
  EFI_HANDLE                        *Handles;
  UINTN                              NumHandles;
  UINTN                              Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   *Fs;
  EFI_FILE_PROTOCOL                 *CandidateRoot;
  EFI_FILE_SYSTEM_INFO              *FsInfo;
  UINTN                              FsInfoSize;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &NumHandles,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (NumHandles == 0)) {
    Print (L"[PROVISION] No filesystem volumes found: %r\n", Status);
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  for (Index = 0; Index < NumHandles; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fs->OpenVolume (Fs, &CandidateRoot);
    if (EFI_ERROR (Status)) {
      continue;
    }

    FsInfoSize = 0;
    Status     = CandidateRoot->GetInfo (CandidateRoot, &gEfiFileSystemInfoGuid, &FsInfoSize, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      CandidateRoot->Close (CandidateRoot);
      continue;
    }

    FsInfo = AllocatePool (FsInfoSize);
    if (FsInfo == NULL) {
      CandidateRoot->Close (CandidateRoot);
      continue;
    }

    Status = CandidateRoot->GetInfo (CandidateRoot, &gEfiFileSystemInfoGuid, &FsInfoSize, FsInfo);
    if (EFI_ERROR (Status) || (StrCmp (FsInfo->VolumeLabel, SHARED_VOLUME_LABEL) != 0)) {
      FreePool (FsInfo);
      CandidateRoot->Close (CandidateRoot);
      continue;
    }

    FreePool (FsInfo);
    FreePool (Handles);
    *Root = CandidateRoot;
    return EFI_SUCCESS;
  }

  FreePool (Handles);
  Print (L"[PROVISION] No filesystem volume labeled \"%s\" found\n", SHARED_VOLUME_LABEL);
  return EFI_NOT_FOUND;
}

/* ── helper: read the update ticket off the shared volume ────────────────
 * The ticket must live outside anything ReadRomImage measures — a
 * firmware volume containing its own certifying ticket is a fixpoint
 * that can never be satisfied. A plain FAT file sidesteps that; see
 * tools/sign_rom_ticket.py, which writes this exact file.
 */
STATIC EFI_STATUS
ReadTicketFile (
  OUT VOID   **TicketBuffer,
  OUT UINTN   *TicketSize
  )
{
  EFI_STATUS          Status;
  EFI_FILE_PROTOCOL   *Root;
  EFI_FILE_PROTOCOL   *File;
  EFI_FILE_INFO        *FileInfo;
  UINTN                FileInfoSize;
  UINTN                ReadSize;

  Status = LocateSharedVolume (&Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Root->Open (Root, &File, UPDATE_TICKET_PATH, EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Open %s failed: %r\n", UPDATE_TICKET_PATH, Status);
    return Status;
  }

  FileInfoSize = 0;
  Status       = File->GetInfo (File, &gEfiFileInfoGuid, &FileInfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    File->Close (File);
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  FileInfo = AllocatePool (FileInfoSize);
  if (FileInfo == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
  if (EFI_ERROR (Status)) {
    FreePool (FileInfo);
    File->Close (File);
    return Status;
  }

  ReadSize = (UINTN)FileInfo->FileSize;
  FreePool (FileInfo);

  *TicketBuffer = AllocatePool (ReadSize);
  if (*TicketBuffer == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->Read (File, &ReadSize, *TicketBuffer);
  File->Close (File);
  if (EFI_ERROR (Status)) {
    FreePool (*TicketBuffer);
    return Status;
  }

  *TicketSize = ReadSize;
  return EFI_SUCCESS;
}

/* ── helper: set a real TPM_RH_OWNER auth (was NULL — issue #15) ───────── */
STATIC EFI_STATUS
SetOwnerAuth (
  VOID
  )
{
  EFI_STATUS  Status;
  TPM2B_AUTH  NewAuth;

  ZeroMem (&NewAuth, sizeof (NewAuth));
  NewAuth.size = sizeof (OWNER_AUTH_STRING) - 1;
  CopyMem (NewAuth.buffer, OWNER_AUTH_STRING, NewAuth.size);

  Status = Tpm2HierarchyChangeAuth (TPM_RH_OWNER, NULL, &NewAuth);
  if (EFI_ERROR (Status)) {
    Print (
      L"[PROVISION] Tpm2HierarchyChangeAuth failed: %r\n"
      L"[PROVISION] Owner auth may already be set from a previous run —\n"
      L"[PROVISION] wipe the TPM state (swtpm dir) and re-provision from clean.\n",
      Status
      );
  }

  return Status;
}

STATIC VOID
BuildOwnerAuthSession (
  OUT TPMS_AUTH_COMMAND  *AuthSession
  )
{
  ZeroMem (AuthSession, sizeof (*AuthSession));
  AuthSession->sessionHandle = TPM_RS_PW;
  AuthSession->hmac.size     = sizeof (OWNER_AUTH_STRING) - 1;
  CopyMem (AuthSession->hmac.buffer, OWNER_AUTH_STRING, AuthSession->hmac.size);
}

/* ── helper: aHash = SHA256(ApprovedPolicy || PolicyRef) ──────────────────
 * This — NOT the raw ApprovedPolicy digest — is what TPM2_PolicyAuthorize
 * actually checks the signature against internally (confirmed against
 * ms-tpm-20-ref's PolicyAuthorize.c: "aHash := hash(approvedPolicy ||
 * policyRef)"), and so what Tpm2VerifySignature must be called with. It's
 * exactly what tools/sign_rom_ticket.py computes and signs offline —
 * this must match it byte-for-byte or the ticket will never verify.
 */
STATIC VOID
ComputeAHash (
  IN  TPM2B_DIGEST  *ApprovedPolicy,
  IN  TPM2B_NONCE   *PolicyRef,
  OUT TPM2B_DIGEST  *AHash
  )
{
  UINT8  Concat[sizeof (ApprovedPolicy->buffer) + sizeof (PolicyRef->buffer)];
  UINTN  ConcatLen;

  CopyMem (Concat, ApprovedPolicy->buffer, ApprovedPolicy->size);
  ConcatLen = ApprovedPolicy->size;
  CopyMem (Concat + ConcatLen, PolicyRef->buffer, PolicyRef->size);
  ConcatLen += PolicyRef->size;

  AHash->size = SHA256_DIGEST_SIZE;
  Sha256HashAll (Concat, ConcatLen, AHash->buffer);
}

/* ── helper: compute the PolicyAuthorize digest via a trial session ──────
 * Unlike the old PCR-only design, this has NO PolicyPCR in it at all —
 * the trial digest depends only on the signing key's Name. That's the
 * whole mechanism: the vault's rule, set here once, never has to change
 * when firmware updates; only the per-update ticket does. See issue #15.
 */
STATIC EFI_STATUS
ComputePolicyAuthorizeDigest (
  OUT TPM2B_DIGEST  *AuthPolicy
  )
{
  EFI_STATUS             Status;
  TPMI_SH_AUTH_SESSION   TrialSession;
  TPM_HANDLE              KeyHandle;
  TPM2B_NAME              KeyName;
  TPM2B_DIGEST            EmptyApprovedPolicy;
  TPM2B_NONCE             EmptyPolicyRef;
  TPMT_TK_VERIFIED        NullTicket;

  Status = OpenPcrPolicySession (TPM_SE_TRIAL, &TrialSession);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] OpenPcrPolicySession (trial) failed: %r\n", Status);
    return Status;
  }

  Status = Tpm2LoadExternal (&gTrustedUpdateKey, &KeyHandle, &KeyName);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2LoadExternal (trial) failed: %r\n", Status);
    Tpm2FlushContext (TrialSession);
    return Status;
  }

  ZeroMem (&EmptyApprovedPolicy, sizeof (EmptyApprovedPolicy));   /* trial session has no prior PolicyPCR step */
  ZeroMem (&EmptyPolicyRef, sizeof (EmptyPolicyRef));
  ZeroMem (&NullTicket, sizeof (NullTicket));
  NullTicket.tag       = TPM_ST_VERIFIED;
  NullTicket.hierarchy = TPM_RH_NULL;   /* trial sessions skip ticket validation entirely */

  Status = Tpm2PolicyAuthorize (TrialSession, &EmptyApprovedPolicy, &EmptyPolicyRef, &KeyName, &NullTicket);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2PolicyAuthorize (trial) failed: %r\n", Status);
    Tpm2FlushContext (KeyHandle);
    Tpm2FlushContext (TrialSession);
    return Status;
  }

  Status = Tpm2PolicyGetDigest (TrialSession, AuthPolicy);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2PolicyGetDigest failed: %r\n", Status);
  }

  Tpm2FlushContext (KeyHandle);
  Tpm2FlushContext (TrialSession);
  return Status;
}

/* ── helper: define the secret's NV index, gated on the PolicyAuthorize
 * digest ───────────────────────────────────────────────────────────────
 * POLICYWRITE|POLICYREAD (not AUTHWRITE/AUTHREAD) means the policy is the
 * only door — there is no password fallback into this index. Owner auth
 * (issue #15) gates who can undefine/redefine the index itself.
 *
 * WRITEDEFINE (issue #9) makes the write side one-shot: TPM2_NV_WriteLock
 * refuses to lock an index that has neither WRITEDEFINE nor
 * WRITE_STCLEAR set, so this attribute is what makes LockSecretNvIndex()
 * below actually do anything. Once locked, anything that satisfies the
 * PCR15+ticket policy can still *read* the secret, but nothing can
 * overwrite it short of TPM_RH_OWNER redefining the index outright — that
 * redefine path is the future OTA reseal flow (#6), not built yet.
 */
STATIC EFI_STATUS
DefineSecretNvIndex (
  IN TPM2B_DIGEST  *PolicyDigest
  )
{
  EFI_STATUS         Status;
  TPM2B_NV_PUBLIC     NvPublic;
  TPM2B_AUTH          IndexAuth;
  TPMS_AUTH_COMMAND   OwnerAuthSession;

  ZeroMem (&NvPublic, sizeof (NvPublic));
  NvPublic.nvPublic.nvIndex   = SECRET_NV_INDEX;
  NvPublic.nvPublic.nameAlg   = TPM_ALG_SHA256;
  NvPublic.nvPublic.attributes.TPMA_NV_POLICYWRITE = 1;
  NvPublic.nvPublic.attributes.TPMA_NV_POLICYREAD  = 1;
  NvPublic.nvPublic.attributes.TPMA_NV_WRITEDEFINE = 1;
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
  BuildOwnerAuthSession (&OwnerAuthSession);

  Status = Tpm2NvDefineSpace (TPM_RH_OWNER, &OwnerAuthSession, &IndexAuth, &NvPublic);
  if (Status == EFI_ALREADY_STARTED) {
    Print (L"[PROVISION] NV index 0x%x already defined — reusing it\n", SECRET_NV_INDEX);
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2NvDefineSpace failed: %r\n", Status);
  }

  return Status;
}

/* ── helper: prove this boot's ROM against its ticket, and hand back a
 * session authorized to touch the NV index ───────────────────────────────
 * Shared by provisioning's write and (once written) TpmVerifyBootApp's
 * read — same chain either way:
 *   PolicyPCR (TPM reads its own live PCR15)
 *   -> GetDigest
 *   -> LoadExternal the compiled-in key
 *   -> read + unmarshal this boot's ticket
 *   -> VerifySignature (does the ticket sign THIS digest?)
 *   -> PolicyAuthorize (swap the session's digest for "approved by KeyName")
 * Not factored into the shared library yet — per this project's own
 * convention (see Tpm2PcrLib / issue #8), duplication gets factored out
 * once it's needed twice for real, not before. TpmVerifyBootApp will be
 * the second, real use.
 */
STATIC EFI_STATUS
RunAuthorizedPolicySession (
  OUT TPMI_SH_AUTH_SESSION  *SessionOut
  )
{
  EFI_STATUS             Status;
  TPMI_SH_AUTH_SESSION   RealSession;
  TPML_PCR_SELECTION     Pcrs;
  TPM2B_DIGEST            EmptyPcrDigest;
  TPM2B_DIGEST            PolicyDigestAfterPcr;
  TPM_HANDLE              KeyHandle;
  TPM2B_NAME              KeyName;
  VOID                    *TicketBuffer;
  UINTN                   TicketSize;
  TPMT_SIGNATURE          Signature;
  TPM2B_DIGEST            AHash;
  TPMT_TK_VERIFIED        CheckTicket;
  TPM2B_NONCE             EmptyPolicyRef;

  Status = OpenPcrPolicySession (TPM_SE_POLICY, &RealSession);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] OpenPcrPolicySession (real) failed: %r\n", Status);
    return Status;
  }

  BuildPcrSelection (PcdGet32 (PcdPcrForBios), &Pcrs);
  ZeroMem (&EmptyPcrDigest, sizeof (EmptyPcrDigest));

  Status = Tpm2PolicyPCR (RealSession, &EmptyPcrDigest, &Pcrs);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2PolicyPCR failed: %r\n", Status);
    Tpm2FlushContext (RealSession);
    return Status;
  }

  Status = Tpm2PolicyGetDigest (RealSession, &PolicyDigestAfterPcr);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2PolicyGetDigest failed: %r\n", Status);
    Tpm2FlushContext (RealSession);
    return Status;
  }

  Status = Tpm2LoadExternal (&gTrustedUpdateKey, &KeyHandle, &KeyName);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2LoadExternal failed: %r\n", Status);
    Tpm2FlushContext (RealSession);
    return Status;
  }

  Status = ReadTicketFile (&TicketBuffer, &TicketSize);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] ReadTicketFile failed: %r — is %s on the shared volume?\n", Status, UPDATE_TICKET_PATH);
    Tpm2FlushContext (KeyHandle);
    Tpm2FlushContext (RealSession);
    return Status;
  }

  if (TicketSize != gTrustedUpdateKey.publicArea.unique.rsa.size) {
    Print (
      L"[PROVISION] Ticket is %u bytes, expected %u (RSA-2048 signature size) — wrong file?\n",
      (UINT32)TicketSize,
      gTrustedUpdateKey.publicArea.unique.rsa.size
      );
    FreePool (TicketBuffer);
    Tpm2FlushContext (KeyHandle);
    Tpm2FlushContext (RealSession);
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Signature, sizeof (Signature));
  Signature.sigAlg                = TPM_ALG_RSASSA;
  Signature.signature.rsassa.hash = TPM_ALG_SHA256;
  Signature.signature.rsassa.sig.size = (UINT16)TicketSize;
  CopyMem (Signature.signature.rsassa.sig.buffer, TicketBuffer, TicketSize);
  FreePool (TicketBuffer);

  /* Same empty PolicyRef feeds both calls below — must match exactly
   * what tools/sign_rom_ticket.py used (empty) in ComputeAHash, and must
   * be the same value passed to Tpm2PolicyAuthorize afterward. */
  ZeroMem (&EmptyPolicyRef, sizeof (EmptyPolicyRef));

  /* Must verify aHash, not the raw PolicyDigestAfterPcr — see ComputeAHash. */
  ComputeAHash (&PolicyDigestAfterPcr, &EmptyPolicyRef, &AHash);

  Status = Tpm2VerifySignature (KeyHandle, &AHash, &Signature, &CheckTicket);
  Tpm2FlushContext (KeyHandle);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2VerifySignature failed: %r — ticket doesn't match this ROM, or is forged\n", Status);
    Tpm2FlushContext (RealSession);
    return Status;
  }

  Status = Tpm2PolicyAuthorize (RealSession, &PolicyDigestAfterPcr, &EmptyPolicyRef, &KeyName, &CheckTicket);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2PolicyAuthorize failed: %r\n", Status);
    Tpm2FlushContext (RealSession);
    return Status;
  }

  *SessionOut = RealSession;
  return EFI_SUCCESS;
}

/* ── helper: fill Secret with real randomness (was the fixed string
 * "hello" — issue #15) ──────────────────────────────────────────────────
 * A hardcoded secret is forgeable by anyone who reads it out of the ROM;
 * randomness generated once, on-device, at N=0 never appears in any ROM
 * for an attacker to fake convincingly.
 *
 * CAVEAT: this platform's DSC (via the inherited OvmfPkgX64.dsc)
 * resolves RngLib to MdeModulePkg/Library/BaseRngLibTimerLib, whose own
 * doc comment says outright: "Do NOT use this on a production system...
 * weak random algorithm" (it's timer-jitter-based, not a hardware TRNG).
 * That's a real limitation on this fix's strength, not just this
 * function's problem — closes the "secret is a string literal anyone
 * can read out of the ROM" hole, but the replacement isn't
 * cryptographically strong either. Fine for demonstrating the
 * mechanism; swap the platform's RngLib mapping before this matters for
 * real.
 */
STATIC EFI_STATUS
GenerateRandomSecret (
  OUT UINT8  *Secret,
  IN  UINTN   SecretLen
  )
{
  UINT64  RandomBytes[2];   /* GetRandomNumber128 fills 16 bytes */

  ASSERT (SecretLen <= sizeof (RandomBytes));

  if (!GetRandomNumber128 (RandomBytes)) {
    DEBUG ((DEBUG_ERROR, "GenerateRandomSecret: GetRandomNumber128 failed\n"));
    return EFI_DEVICE_ERROR;
  }

  CopyMem (Secret, RandomBytes, SecretLen);
  return EFI_SUCCESS;
}

/* ── helper: lock the secret's NV index against further writes (issue #9)
 * ─────────────────────────────────────────────────────────────────────
 * TPM_RH_OWNER authorizes the lock directly — this is deliberately
 * independent of the index's own PolicyAuthorize gate, so locking doesn't
 * require re-proving this boot's ROM. Once TPM2_NV_WriteLock sets
 * TPMA_NV_WRITELOCKED, it stays set until the index is undefined and
 * redefined (owner-authorized, same as DefineSecretNvIndex above) — there
 * is no "unlock" command.
 */
STATIC EFI_STATUS
LockSecretNvIndex (
  VOID
  )
{
  EFI_STATUS         Status;
  TPMS_AUTH_COMMAND   OwnerAuthSession;

  BuildOwnerAuthSession (&OwnerAuthSession);

  Status = Tpm2NvWriteLock (TPM_RH_OWNER, SECRET_NV_INDEX, &OwnerAuthSession);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2NvWriteLock failed: %r\n", Status);
  } else {
    Print (L"[PROVISION] NV index 0x%x write-locked — no further writes until redefined\n", SECRET_NV_INDEX);
  }

  return Status;
}

/* ── helper: write the secret using an already-authorized session ──────── */
STATIC EFI_STATUS
WriteSecretToNvIndex (
  IN TPMI_SH_AUTH_SESSION  Session,
  IN UINT8                 *Secret,
  IN UINTN                  SecretLen
  )
{
  EFI_STATUS           Status;
  TPMS_AUTH_COMMAND     AuthSession;
  TPM2B_MAX_BUFFER      InData;

  ZeroMem (&AuthSession, sizeof (AuthSession));
  AuthSession.sessionHandle = Session;   /* nonce/hmac left empty — no password on this index */

  ZeroMem (&InData, sizeof (InData));
  InData.size = (UINT16)SecretLen;
  CopyMem (InData.buffer, Secret, SecretLen);

  Status = Tpm2NvWrite (SECRET_NV_INDEX, SECRET_NV_INDEX, &AuthSession, &InData, 0);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2NvWrite failed: %r\n", Status);
  } else {
    Print (L"[PROVISION] Secret written to NV index 0x%x, gated on PolicyAuthorize\n", SECRET_NV_INDEX);
  }

  Tpm2FlushContext (Session);

  if (!EFI_ERROR (Status)) {
    Status = LockSecretNvIndex ();
  }

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
  EFI_STATUS             Status;
  VOID                   *RomBuffer = NULL;
  UINTN                   RomSize   = 0;
  TPM2B_DIGEST            AuthPolicy;
  TPMI_SH_AUTH_SESSION    AuthorizedSession;
  UINT8                   Secret[SECRET_LEN];

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

  /* Step 3: Measure ROM -> extend PCR[15] */
  Print (L"[PROVISION] Reading ROM image...\n");
  Status = ReadRomImage (&RomBuffer, &RomSize);
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] ReadRomImage failed: %r\n", Status);
    return Status;
  }

  Print (L"[PROVISION] Extending PCR[%u]...\n", PcdGet32 (PcdPcrForBios));
  {
    TPML_DIGEST  Digests;

    Status = ExtendPcr (Tcg2, PcdGet32 (PcdPcrForBios), RomBuffer, RomSize, "BIOS ROM", &Digests);
    if (EFI_ERROR (Status)) {
      Print (L"[PROVISION] ExtendPcr failed: %r\n", Status);
      FreePool (RomBuffer);
      return Status;
    }

    if (Digests.count > 0) {
      UINT32  i;

      Print (L"[PROVISION] PCR[%u] = ", PcdGet32 (PcdPcrForBios));
      for (i = 0; i < Digests.digests[0].size; i++) {
        Print (L"%02x", Digests.digests[0].buffer[i]);
      }

      Print (L"\n");
    }
  }
  Print (L"\n");

  Status = Tpm2RequestUseTpm ();
  if (EFI_ERROR (Status)) {
    Print (L"[PROVISION] Tpm2RequestUseTpm failed: %r\n", Status);
    FreePool (RomBuffer);
    return Status;
  }

  /* Step 4: real owner auth (issue #15 — was NULL) */
  Status = SetOwnerAuth ();
  if (EFI_ERROR (Status)) {
    FreePool (RomBuffer);
    return Status;
  }

  /* Step 5: PolicyAuthorize trial digest, keyed to the compiled-in signing key */
  Print (L"[PROVISION] Computing PolicyAuthorize digest...\n");
  Status = ComputePolicyAuthorizeDigest (&AuthPolicy);
  if (EFI_ERROR (Status)) {
    FreePool (RomBuffer);
    return Status;
  }

  /* Step 6: define the NV index, gated on that digest — fixed forever after this */
  Status = DefineSecretNvIndex (&AuthPolicy);
  if (EFI_ERROR (Status)) {
    FreePool (RomBuffer);
    return Status;
  }

  /* Step 7: prove this boot's ROM against its ticket, then write a random secret */
  Print (L"[PROVISION] Verifying this boot's ROM against its ticket...\n");
  Status = RunAuthorizedPolicySession (&AuthorizedSession);
  if (EFI_ERROR (Status)) {
    FreePool (RomBuffer);
    return Status;
  }

  Status = GenerateRandomSecret (Secret, SECRET_LEN);
  if (EFI_ERROR (Status)) {
    Tpm2FlushContext (AuthorizedSession);
    FreePool (RomBuffer);
    return Status;
  }

  Status = WriteSecretToNvIndex (AuthorizedSession, Secret, SECRET_LEN);
  FreePool (RomBuffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Print (L"\n=== TpmProvisionApp done (%r) ===\n\n", Status);
  return Status;
}
