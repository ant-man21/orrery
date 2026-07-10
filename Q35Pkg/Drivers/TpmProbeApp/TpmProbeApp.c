/**
 * TpmProbeApp.c
 *
 * Step 1 proof-of-concept:
 *   1. Locate EFI_TCG2_PROTOCOL, print capabilities
 *   2. Read the running BIOS image from flash (via EFI_FIRMWARE_VOLUME2_PROTOCOL)
 *      and extend PCR[16] with its hash
 *   3. Seal the string "hello" to the current PCR[16] value
 *   4. Immediately unseal it to prove the round-trip works
 *   5. Save the raw ROM image to \dummy.fd on the ESP for later comparison
 *
 * Build: UEFI application (not a driver). Load it from the UEFI shell:
 *   Shell> fs0:\TpmProbeApp.efi
 */

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/Tpm2CommandLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>

#include <Protocol/Tcg2Protocol.h>
#include <Protocol/SimpleFileSystem.h>
// #include <Protocol/FirmwareVolume2.h>

#include <Library/Tpm2CommandLib.h>
#include <IndustryStandard/Tpm20.h>
#include <IndustryStandard/TpmPtp.h>
/* ── constants ─────────────────────────────────────────────────────────── */

#define PCR_FOR_BIOS      16          /* user-controlled, safe to extend    */
#define DUMMY_FD_PATH     L"\\dummy.fd"
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
    Print (L"[PROBE] TCG2 protocol not found: %r\n", Status);
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
    Print (L"[PROBE] GetCapability failed: %r\n", Status);
    return;
  }

  Print (L"[PROBE] TPM present         : %s\n",
         Cap.TPMPresentFlag ? L"YES" : L"NO");
  Print (L"[PROBE] Active PCR banks    : 0x%08x\n",
         Cap.ActivePcrBanks);
  Print (L"[PROBE] TPM2 supported      : %s\n",
         (Cap.ProtocolVersion.Major >= 1) ? L"YES" : L"NO");
}

/* ── helper: read current ROM from flash ───────────────────────────────── */
/*
 * In QEMU/OVMF the firmware runs from memory-mapped flash.
 * EFI_FIRMWARE_VOLUME2_PROTOCOL gives us access to firmware volumes.
 * For a quick probe we just grab the first FV handle and hash the whole
 * range – replace this with your platform's ROM base if needed.
 *
 * Returns an AllocatePool'd buffer; caller must FreePool it.
 */
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
  Print (L"[PROBE] ROM snapshot: 0xFFC00000, %u bytes\n", (UINT32)FlashSize);
  return EFI_SUCCESS;
}

/* ── helper: extend PCR[16] with arbitrary data ─────────────────────────── */

STATIC EFI_STATUS
ExtendPcr16 ( //something fails here. we get assert
  IN EFI_TCG2_PROTOCOL  *Tcg2,
  IN VOID               *Data,
  IN UINTN               DataSize
  )
{
  EFI_STATUS                 Status;
  EFI_TCG2_EVENT            *Event;
  UINTN                      EventSize;
  UINT32                     UpdateCounter;
  // TPML_PCR_SELECTION         PcrSelectionOut
  // UINT8                      Hash[SHA256_DIGEST_SIZE];

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
    Print (L"[PROBE] HashLogExtendEvent failed: %r\n", Status);
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
    ZeroMem (&In, sizeof (In));
    In.count = 1;
    In.pcrSelections[0] = PcrSel;
    DEBUG ((DEBUG_INFO, "ExtendPcr16 [3]\n"));

    // Status = Tpm2PcrRead (&In, NULL, &Digests);
    Status = Tpm2PcrRead (&In, &UpdateCounter, NULL, &Digests);
    DEBUG ((DEBUG_INFO, "ExtendPcr16 [4]\n"));
    if (!EFI_ERROR (Status) && Digests.count > 0) {
      UINT32 i;
      Print (L"[PROBE] PCR[%u] = ", PCR_FOR_BIOS);
      for (i = 0; i < Digests.digests[0].size; i++) {
        Print (L"%02x", Digests.digests[0].buffer[i]);
      }
      Print (L"\n");
    }
  }

  return EFI_SUCCESS;
}

/* ── helper: save buffer to a file on fs0: ─────────────────────────────── */

STATIC EFI_STATUS
SaveFileToDisk (
  IN CHAR16  *FileName,
  IN VOID    *Data,
  IN UINTN    DataSize
  )
{
  EFI_STATUS                        Status;
  UINTN                             NumHandles;
  EFI_HANDLE                       *Handles;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  UINTN                             WriteSize;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &NumHandles,
                  &Handles
                  );
  if (EFI_ERROR (Status) || NumHandles == 0) {
    Print (L"[PROBE] No filesystem found: %r\n", Status);
    return EFI_NOT_FOUND;
  }

  /* Use the first filesystem (fs0:) */
  Status = gBS->HandleProtocol (
                  Handles[0],
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Fs
                  );
  FreePool (Handles);
  if (EFI_ERROR (Status)) return Status;

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) return Status;

  Status = Root->Open (
                   Root, &File, FileName,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   0
                   );
  if (EFI_ERROR (Status)) {
    Root->Close (Root);
    Print (L"[PROBE] Cannot create %s: %r\n", FileName, Status);
    return Status;
  }

  WriteSize = DataSize;
  Status    = File->Write (File, &WriteSize, Data);
  File->Close (File);
  Root->Close (Root);

  if (EFI_ERROR (Status)) {
    Print (L"[PROBE] Write failed: %r\n", Status);
  } else {
    Print (L"[PROBE] Saved %u bytes to %s\n", (UINT32)WriteSize, FileName);
  }
  return Status;
}

/* ── helper: seal / unseal round-trip ──────────────────────────────────── */
/*
 * Sealing binds a secret to the current PCR state via a TPM2 policy.
 * High-level flow:
 *   1. Start an auth session with a PCR policy (PolicyPCR on PCR[16])
 *   2. Tpm2CreateSealed  → produces (outPublic, outPrivate) sealed object
 *   3. Tpm2Load          → loads the object into the TPM, returns handle
 *   4. Tpm2Unseal        → reads the plaintext back (only works if PCR[16]
 *                          matches the policy digest used at seal time)
 *
 * Tpm2CommandLib exposes Tpm2CreateSealed / Tpm2Unseal as high-level
 * wrappers; the session management is handled internally.
 */

// STATIC EFI_STATUS
// SealUnsealRoundTrip (
//   IN UINT8   *Secret,
//   IN UINTN    SecretLen
//   )
// {
//   EFI_STATUS          Status;

//   /* --- buffers for the sealed object ------------------------------------ */
//   TPM2B_PRIVATE  OutPrivate;
//   TPM2B_PUBLIC   OutPublic;
//   ZeroMem (&OutPrivate, sizeof (OutPrivate));
//   ZeroMem (&OutPublic,  sizeof (OutPublic));

//   /* --- parent handle: use the TPM's Storage Primary Seed (SPS) --------- */
//   /* The primary key under the Storage hierarchy is created fresh each     */
//   /* boot unless you persist it.  For the probe we create a transient one. */
//   TPMI_DH_OBJECT  ParentHandle;

//   {
//     TPM2B_SENSITIVE_CREATE  InSensitive;
//     TPM2B_PUBLIC            InPublic;
//     TPM2B_DATA              OutsideInfo;
//     TPML_PCR_SELECTION      CreationPCR;
//     TPM2B_PUBLIC            OutParentPublic;
//     TPM2B_CREATION_DATA     CreationData;
//     TPM2B_DIGEST            CreationHash;
//     TPMT_TK_CREATION        CreationTicket;

//     ZeroMem (&InSensitive,    sizeof (InSensitive));
//     ZeroMem (&InPublic,       sizeof (InPublic));
//     ZeroMem (&OutsideInfo,    sizeof (OutsideInfo));
//     ZeroMem (&CreationPCR,    sizeof (CreationPCR));
//     ZeroMem (&OutParentPublic,sizeof (OutParentPublic));
//     ZeroMem (&CreationData,   sizeof (CreationData));
//     ZeroMem (&CreationHash,   sizeof (CreationHash));
//     ZeroMem (&CreationTicket, sizeof (CreationTicket));

//     /* RSA-2048 storage parent template */
//     InPublic.publicArea.type                  = TPM_ALG_RSA;
//     InPublic.publicArea.nameAlg               = TPM_ALG_SHA256;
//     InPublic.publicArea.objectAttributes.restricted = 1;
//     InPublic.publicArea.objectAttributes.decrypt = 1;
//     InPublic.publicArea.objectAttributes.fixedTPM = 1;
//     InPublic.publicArea.objectAttributes.fixedParent = 1;
//     InPublic.publicArea.objectAttributes.sensitiveDataOrigin = 1;
//     InPublic.publicArea.objectAttributes.userWithAuth = 1;
//     InPublic.publicArea.parameters.rsaDetail.symmetric.algorithm = TPM_ALG_AES;
//     InPublic.publicArea.parameters.rsaDetail.symmetric.keyBits.aes = 128;
//     InPublic.publicArea.parameters.rsaDetail.symmetric.mode.aes    = TPM_ALG_CFB;
//     InPublic.publicArea.parameters.rsaDetail.scheme.scheme         = TPM_ALG_NULL;
//     InPublic.publicArea.parameters.rsaDetail.keyBits               = 2048;
//     InPublic.publicArea.unique.rsa.size                            = 0;

//     Status = Tpm2CreatePrimary (
//                TPM_RH_OWNER,
//                &InSensitive,
//                &InPublic,
//                &OutsideInfo,
//                &CreationPCR,
//                &ParentHandle,
//                &OutParentPublic,
//                &CreationData,
//                &CreationHash,
//                &CreationTicket
//                );
//     if (EFI_ERROR (Status)) {
//       Print (L"[PROBE] Tpm2CreatePrimary failed: %r\n", Status);
//       return Status;
//     }
//     Print (L"[PROBE] Storage primary created, handle=0x%08x\n", ParentHandle);
//   }

//   /* --- build a PCR policy digest for PCR[16] SHA-256 ------------------- */
//   /* A sealed object's authPolicy is a hash of the TPM2_PolicyPCR command  */
//   /* inputs.  We compute it manually here so we can pass it to Create.     */
//   TPM2B_DIGEST  PolicyDigest;
//   {
//     TPML_PCR_SELECTION  PcrSel;
//     ZeroMem (&PcrSel, sizeof (PcrSel));
//     PcrSel.count                              = 1;
//     PcrSel.pcrSelections[0].hash             = TPM_ALG_SHA256;
//     PcrSel.pcrSelections[0].sizeofSelect     = 3;
//     PcrSel.pcrSelections[0].pcrSelect[PCR_FOR_BIOS / 8] =
//       (UINT8)(1 << (PCR_FOR_BIOS % 8));

//     Status = Tpm2ComputePolicyPCRDigest (
//                TPM_ALG_SHA256,
//                &PcrSel,
//                &PolicyDigest
//                );
//     if (EFI_ERROR (Status)) {
//       Print (L"[PROBE] PolicyPCR digest failed: %r\n", Status);
//       Tpm2FlushContext (ParentHandle);
//       return Status;
//     }
//     Print (L"[PROBE] PolicyPCR digest computed (%u bytes)\n",
//            PolicyDigest.size);
//   }

//   /* --- seal "hello" to PCR[16] ----------------------------------------- */
//   {
//     TPM2B_SENSITIVE_CREATE  InSensitive;
//     TPM2B_PUBLIC            InPublic;
//     TPM2B_DATA              OutsideInfo;
//     TPML_PCR_SELECTION      CreationPCR;
//     TPM2B_CREATION_DATA     CreationData;
//     TPM2B_DIGEST            CreationHash;
//     TPMT_TK_CREATION        CreationTicket;

//     ZeroMem (&InSensitive,   sizeof (InSensitive));
//     ZeroMem (&InPublic,      sizeof (InPublic));
//     ZeroMem (&OutsideInfo,   sizeof (OutsideInfo));
//     ZeroMem (&CreationPCR,   sizeof (CreationPCR));
//     ZeroMem (&CreationData,  sizeof (CreationData));
//     ZeroMem (&CreationHash,  sizeof (CreationHash));
//     ZeroMem (&CreationTicket,sizeof (CreationTicket));

//     /* the secret goes in InSensitive.sensitive.data */
//     InSensitive.sensitive.data.size = (UINT16)SecretLen;
//     CopyMem (InSensitive.sensitive.data.buffer, Secret, SecretLen);

//     /* keyedHash (sealed data) template */
//     InPublic.publicArea.type    = TPM_ALG_KEYEDHASH;
//     InPublic.publicArea.nameAlg = TPM_ALG_SHA256;
//     InPublic.publicArea.objectAttributes.fixedTPM = 1;
//     InPublic.publicArea.objectAttributes.fixedParent = 1;
//     InPublic.publicArea.objectAttributes.adminWithPolicy = 1;
//     InPublic.publicArea.authPolicy = PolicyDigest;
//     InPublic.publicArea.parameters.keyedHashDetail.scheme.scheme = TPM_ALG_NULL;

//     Status = Tpm2Create (
//                ParentHandle,
//                &InSensitive,
//                &InPublic,
//                &OutsideInfo,
//                &CreationPCR,
//                &OutPrivate,
//                &OutPublic,
//                &CreationData,
//                &CreationHash,
//                &CreationTicket
//                );
//     if (EFI_ERROR (Status)) {
//       Print (L"[PROBE] Tpm2Create (seal) failed: %r\n", Status);
//       Tpm2FlushContext (ParentHandle);
//       return Status;
//     }
//     Print (L"[PROBE] Secret sealed! private blob %u bytes, public %u bytes\n",
//            OutPrivate.size, OutPublic.size);
//   }

//   /* --- load the sealed object ------------------------------------------ */
//   TPMI_DH_OBJECT  SealedHandle;
//   {
//     TPM2B_NAME  OutName;
//     ZeroMem (&OutName, sizeof (OutName));

//     Status = Tpm2Load (
//                ParentHandle,
//                &OutPrivate,
//                &OutPublic,
//                &SealedHandle,
//                &OutName
//                );
//     if (EFI_ERROR (Status)) {
//       Print (L"[PROBE] Tpm2Load failed: %r\n", Status);
//       Tpm2FlushContext (ParentHandle);
//       return Status;
//     }
//     Print (L"[PROBE] Sealed object loaded, handle=0x%08x\n", SealedHandle);
//   }

//   /* --- open a PCR policy session and unseal ----------------------------- */
//   {
//     TPMI_SH_AUTH_SESSION  SessionHandle;
//     TPM2B_NONCE           NonceCaller;
//     TPM2B_ENCRYPTED_SECRET EncryptedSalt;
//     TPMT_SYM_DEF           Symmetric;
//     TPM2B_NONCE            NonceTPM;

//     ZeroMem (&NonceCaller,    sizeof (NonceCaller));
//     ZeroMem (&EncryptedSalt,  sizeof (EncryptedSalt));
//     ZeroMem (&Symmetric,      sizeof (Symmetric));
//     ZeroMem (&NonceTPM,       sizeof (NonceTPM));

//     NonceCaller.size = 20;                          /* 20-byte nonce       */
//     Tpm2GetRandom (20, NonceCaller.buffer);
//     Symmetric.algorithm = TPM_ALG_NULL;

//     Status = Tpm2StartAuthSession (
//                TPM_RH_NULL,          /* tpmKey   */
//                TPM_RH_NULL,          /* bind     */
//                &NonceCaller,
//                &EncryptedSalt,
//                TPM_SE_POLICY,
//                &Symmetric,
//                TPM_ALG_SHA256,
//                &SessionHandle,
//                &NonceTPM
//                );
//     if (EFI_ERROR (Status)) {
//       Print (L"[PROBE] Tpm2StartAuthSession failed: %r\n", Status);
//       goto Cleanup;
//     }

//     /* run PolicyPCR to satisfy the sealed object's authPolicy */
//     TPML_PCR_SELECTION  PcrSel;
//     ZeroMem (&PcrSel, sizeof (PcrSel));
//     PcrSel.count                           = 1;
//     PcrSel.pcrSelections[0].hash           = TPM_ALG_SHA256;
//     PcrSel.pcrSelections[0].sizeofSelect   = 3;
//     PcrSel.pcrSelections[0].pcrSelect[PCR_FOR_BIOS / 8] =
//       (UINT8)(1 << (PCR_FOR_BIOS % 8));

//     Status = Tpm2PolicyPCR (
//                SessionHandle,
//                NULL,        /* pcrDigest – NULL means "use current PCR"  */
//                &PcrSel
//                );
//     if (EFI_ERROR (Status)) {
//       Print (L"[PROBE] Tpm2PolicyPCR failed: %r\n", Status);
//       Tpm2FlushContext (SessionHandle);
//       goto Cleanup;
//     }

//     /* now unseal */
//     TPM2B_SENSITIVE_DATA  OutData;
//     ZeroMem (&OutData, sizeof (OutData));

//     Status = Tpm2Unseal (SealedHandle, SessionHandle, &OutData);
//     Tpm2FlushContext (SessionHandle);

//     if (EFI_ERROR (Status)) {
//       Print (L"[PROBE] Tpm2Unseal FAILED: %r  ← expected if PCR changed\n",
//              Status);
//     } else {
//       /* compare with original secret */
//       BOOLEAN Match =
//         (OutData.size == SecretLen) &&
//         (CompareMem (OutData.buffer, Secret, SecretLen) == 0);

//       Print (L"[PROBE] Unseal SUCCESS! recovered: \"");
//       for (UINTN i = 0; i < OutData.size; i++) {
//         Print (L"%c", (CHAR16)OutData.buffer[i]);
//       }
//       Print (L"\"  match=%s\n", Match ? L"YES ✓" : L"NO ✗");
//     }
//   }

// Cleanup:
//   Tpm2FlushContext (SealedHandle);
//   Tpm2FlushContext (ParentHandle);
//   return Status;
// }

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

  Print (L"\n=== TpmProbeApp ===\n\n");
  DEBUG((DEBUG_INFO,"TpmProbeApp start...\n")); //why do I not get this debug...
  /* 1. Find TCG2 and print caps */
  Tcg2 = FindTcg2Protocol ();
  if (Tcg2 == NULL) {
    return EFI_NOT_FOUND;
  }
  PrintTcg2Caps (Tcg2);
  Print (L"\n");

  /* 2. Read ROM image */
  Print (L"[PROBE] Reading ROM image...\n");
  Status = ReadRomImage (&RomBuffer, &RomSize);
  if (EFI_ERROR (Status)) {
    Print (L"[PROBE] ReadRomImage failed: %r\n", Status);
    return Status;
  }

  /* 3. Extend PCR[16] with ROM data */
  Print (L"[PROBE] Extending PCR[%u]...\n", PCR_FOR_BIOS);
  Status = ExtendPcr16 (Tcg2, RomBuffer, RomSize);
  if (EFI_ERROR (Status)) {
    FreePool (RomBuffer);
    return Status;
  }
  Print (L"\n");

  /* 4. Save ROM snapshot to disk as dummy.fd */
  Print (L"[PROBE] Saving ROM snapshot to " DUMMY_FD_PATH L"...\n");
  Status = SaveFileToDisk (DUMMY_FD_PATH, RomBuffer, RomSize);
  if (EFI_ERROR (Status)) {
    Print (L"[PROBE] Warning: could not save dummy.fd (%r) – continuing\n",
           Status);
  }
  Print (L"\n");

  /* 5. Seal/Unseal round-trip */
  Print (L"[PROBE] Running seal→unseal round-trip with secret \""
         SECRET_STRING L"\"...\n");
  // Status = SealUnsealRoundTrip (
  //            (UINT8 *)SECRET_STRING,
  //            SECRET_LEN
  //            );

  FreePool (RomBuffer);

  Print (L"\n=== TpmProbeApp done (%r) ===\n\n", Status);
  return Status;
}