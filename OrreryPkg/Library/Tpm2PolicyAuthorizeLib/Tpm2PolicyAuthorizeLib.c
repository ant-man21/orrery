/** @file
  Implements Tpm2LoadExternal / Tpm2VerifySignature / Tpm2PolicyAuthorize —
  see Include/Library/Tpm2PolicyAuthorizeLib.h for why these live here
  instead of edk2/SecurityPkg/Library/Tpm2CommandLib.

  Marshalling pattern (manual byte-cursor construction into a generously
  max-sized #pragma pack(1) command/response scaffold, backfilling
  variable-length TPM2B size prefixes once known) matches
  Tpm2PolicyPcrLib.c and edk2's own Tpm2Session.c / Tpm2Object.c exactly —
  RSA TPMT_PUBLIC field order cross-checked against Tpm2Object.c's
  Tpm2ReadPublic; handle-returned-before-parameters response layout
  cross-checked against Tpm2Session.c's Tpm2StartAuthSession.
**/

#include <Library/Tpm2PolicyAuthorizeLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>

#pragma pack(1)

typedef struct {
  TPM2_COMMAND_HEADER    Header;
  TPM2B_SENSITIVE        InPrivate;   // scratch — this library always marshals it empty
  TPM2B_PUBLIC           InPublic;
  TPMI_RH_HIERARCHY      Hierarchy;
} TPM2_LOAD_EXTERNAL_COMMAND;

typedef struct {
  TPM2_RESPONSE_HEADER    Header;
  TPM_HANDLE               ObjectHandle;
  TPM2B_NAME               Name;
} TPM2_LOAD_EXTERNAL_RESPONSE;

typedef struct {
  TPM2_COMMAND_HEADER    Header;
  TPMI_DH_OBJECT          KeyHandle;
  TPM2B_DIGEST            Digest;
  TPMT_SIGNATURE          Signature;
} TPM2_VERIFY_SIGNATURE_COMMAND;

typedef struct {
  TPM2_RESPONSE_HEADER    Header;
  TPMT_TK_VERIFIED         Validation;
} TPM2_VERIFY_SIGNATURE_RESPONSE;

typedef struct {
  TPM2_COMMAND_HEADER    Header;
  TPMI_SH_POLICY          PolicySession;
  TPM2B_DIGEST            ApprovedPolicy;
  TPM2B_NONCE             PolicyRef;
  TPM2B_NAME              KeySign;
  TPMT_TK_VERIFIED         CheckTicket;
} TPM2_POLICY_AUTHORIZE_COMMAND;

typedef struct {
  TPM2_RESPONSE_HEADER    Header;
} TPM2_POLICY_AUTHORIZE_RESPONSE;

#pragma pack()

/**
  Marshal an RSA-only TPMT_PUBLIC's publicArea onto the wire. Only
  supports the exact shape TrustedUpdateKey.h produces (symmetric=NULL,
  scheme=NULL) — anything else is a caller bug, not a case this project
  needs to handle, so it's asserted rather than branched on.

  @return Buffer, advanced past the bytes just written.
**/
STATIC UINT8 *
MarshalRsaPublicArea (
  IN  TPM2B_PUBLIC  *Public,
  OUT UINT8         *Buffer
  )
{
  ASSERT (Public->publicArea.type == TPM_ALG_RSA);
  ASSERT (Public->publicArea.parameters.rsaDetail.symmetric.algorithm == TPM_ALG_NULL);
  ASSERT (Public->publicArea.parameters.rsaDetail.scheme.scheme == TPM_ALG_NULL);

  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Public->publicArea.type));
  Buffer += sizeof (UINT16);
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Public->publicArea.nameAlg));
  Buffer += sizeof (UINT16);
  WriteUnaligned32 (
    (UINT32 *)Buffer,
    SwapBytes32 (ReadUnaligned32 ((UINT32 *)&Public->publicArea.objectAttributes))
    );
  Buffer += sizeof (UINT32);

  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Public->publicArea.authPolicy.size));
  Buffer += sizeof (UINT16);
  CopyMem (Buffer, Public->publicArea.authPolicy.buffer, Public->publicArea.authPolicy.size);
  Buffer += Public->publicArea.authPolicy.size;

  // TPMS_RSA_PARMS
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Public->publicArea.parameters.rsaDetail.symmetric.algorithm));
  Buffer += sizeof (UINT16);
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Public->publicArea.parameters.rsaDetail.scheme.scheme));
  Buffer += sizeof (UINT16);
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Public->publicArea.parameters.rsaDetail.keyBits));
  Buffer += sizeof (UINT16);
  WriteUnaligned32 ((UINT32 *)Buffer, SwapBytes32 (Public->publicArea.parameters.rsaDetail.exponent));
  Buffer += sizeof (UINT32);

  // TPMU_PUBLIC_ID (rsa)
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Public->publicArea.unique.rsa.size));
  Buffer += sizeof (UINT16);
  CopyMem (Buffer, Public->publicArea.unique.rsa.buffer, Public->publicArea.unique.rsa.size);
  Buffer += Public->publicArea.unique.rsa.size;

  return Buffer;
}

EFI_STATUS
EFIAPI
Tpm2LoadExternal (
  IN  TPM2B_PUBLIC  *InPublic,
  OUT TPM_HANDLE    *ObjectHandle,
  OUT TPM2B_NAME    *Name
  )
{
  EFI_STATUS                     Status;
  TPM2_LOAD_EXTERNAL_COMMAND     SendBuffer;
  TPM2_LOAD_EXTERNAL_RESPONSE    RecvBuffer;
  UINT32                         SendBufferSize;
  UINT32                         RecvBufferSize;
  UINT8                          *Buffer;
  UINT8                          *PublicSizeField;
  UINT8                          *PublicStart;
  UINT16                         NameSize;

  //
  // Construct command
  //
  SendBuffer.Header.tag         = SwapBytes16 (TPM_ST_NO_SESSIONS);
  SendBuffer.Header.commandCode = SwapBytes32 (TPM_CC_LoadExternal);

  Buffer = (UINT8 *)&SendBuffer.InPrivate;

  // inPrivate — always empty. This library only ever loads a public key,
  // never a private/sensitive object.
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (0));
  Buffer += sizeof (UINT16);

  // inPublic — size-prefixed; the prefix is backfilled once the RSA
  // public area is marshalled, since its length isn't known up front.
  PublicSizeField = Buffer;
  Buffer         += sizeof (UINT16);
  PublicStart      = Buffer;
  Buffer           = MarshalRsaPublicArea (InPublic, Buffer);
  WriteUnaligned16 ((UINT16 *)PublicSizeField, SwapBytes16 ((UINT16)(Buffer - PublicStart)));

  // hierarchy — TPM_RH_OWNER, not TPM_RH_NULL. This key is never
  // persisted either way (loaded transiently, flushed after use), but
  // the hierarchy also controls what kind of ticket Tpm2VerifySignature
  // can later produce for objects loaded here: real hardware confirmed
  // that TPM_RH_NULL gets you a placeholder ticket back (hierarchy =
  // TPM_RH_NULL, digest.size = 0) instead of a real one, because NULL
  // is the "anyone can load anything, no persistent identity" hierarchy
  // — a genuine verification ticket from it would be meaningless as
  // proof. That placeholder is exactly right for a *trial* session's
  // throwaway ticket (which skips validation entirely), but a *real*
  // Tpm2PolicyAuthorize call rejects it outright — TPM_RC_VALUE on the
  // checkTicket parameter, confirmed against real swtpm hardware.
  // TPM_RH_OWNER matches the standard convention for this exact
  // workflow (e.g. `tpm2_loadexternal`'s own default is `-C o`, Owner
  // hierarchy) and needs no owner authorization here — LoadExternal
  // takes no auth session regardless of which hierarchy is named. Name
  // computation is unaffected either way: an object's Name is purely a
  // function of its public area content, never the loading hierarchy.
  WriteUnaligned32 ((UINT32 *)Buffer, SwapBytes32 (TPM_RH_OWNER));
  Buffer += sizeof (UINT32);

  SendBufferSize              = (UINT32)((UINTN)Buffer - (UINTN)&SendBuffer);
  SendBuffer.Header.paramSize = SwapBytes32 (SendBufferSize);

  //
  // send Tpm command
  //
  RecvBufferSize = sizeof (RecvBuffer);
  Status         = Tpm2SubmitCommand (SendBufferSize, (UINT8 *)&SendBuffer, &RecvBufferSize, (UINT8 *)&RecvBuffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (RecvBufferSize < sizeof (TPM2_RESPONSE_HEADER) + sizeof (TPM_HANDLE)) {
    DEBUG ((DEBUG_ERROR, "Tpm2LoadExternal - RecvBufferSize Error - %x\n", RecvBufferSize));
    return EFI_DEVICE_ERROR;
  }

  if (SwapBytes32 (RecvBuffer.Header.responseCode) != TPM_RC_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "Tpm2LoadExternal - responseCode - %x\n", SwapBytes32 (RecvBuffer.Header.responseCode)));
    return EFI_DEVICE_ERROR;
  }

  *ObjectHandle = SwapBytes32 (ReadUnaligned32 ((UINT32 *)&RecvBuffer.ObjectHandle));

  NameSize = SwapBytes16 (ReadUnaligned16 ((UINT16 *)&RecvBuffer.Name.size));
  if ((NameSize > sizeof (TPMU_NAME)) ||
      (RecvBufferSize != sizeof (TPM2_RESPONSE_HEADER) + sizeof (TPM_HANDLE) + sizeof (UINT16) + NameSize))
  {
    DEBUG ((DEBUG_ERROR, "Tpm2LoadExternal - NameSize error %x\n", NameSize));
    return EFI_DEVICE_ERROR;
  }

  Name->size = NameSize;
  CopyMem (Name->name, RecvBuffer.Name.name, NameSize);

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Tpm2VerifySignature (
  IN  TPM_HANDLE         KeyHandle,
  IN  TPM2B_DIGEST       *Digest,
  IN  TPMT_SIGNATURE     *Signature,
  OUT TPMT_TK_VERIFIED   *Validation
  )
{
  EFI_STATUS                        Status;
  TPM2_VERIFY_SIGNATURE_COMMAND     SendBuffer;
  TPM2_VERIFY_SIGNATURE_RESPONSE    RecvBuffer;
  UINT32                            SendBufferSize;
  UINT32                            RecvBufferSize;
  UINT8                             *Buffer;
  UINT16                            TicketDigestSize;

  if (Signature->sigAlg != TPM_ALG_RSASSA) {
    // This library only speaks RSASSA — matches TrustedUpdateKey.h's
    // scheme=NULL object and sign_rom_ticket.py's PKCS1v1.5 tickets.
    DEBUG ((DEBUG_ERROR, "Tpm2VerifySignature - unsupported sigAlg %x (only RSASSA)\n", Signature->sigAlg));
    return EFI_UNSUPPORTED;
  }

  //
  // Construct command
  //
  SendBuffer.Header.tag         = SwapBytes16 (TPM_ST_NO_SESSIONS);
  SendBuffer.Header.commandCode = SwapBytes32 (TPM_CC_VerifySignature);

  SendBuffer.KeyHandle = SwapBytes32 (KeyHandle);

  Buffer = (UINT8 *)&SendBuffer.Digest;
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Digest->size));
  Buffer += sizeof (UINT16);
  CopyMem (Buffer, Digest->buffer, Digest->size);
  Buffer += Digest->size;

  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Signature->sigAlg));
  Buffer += sizeof (UINT16);
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Signature->signature.rsassa.hash));
  Buffer += sizeof (UINT16);
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Signature->signature.rsassa.sig.size));
  Buffer += sizeof (UINT16);
  CopyMem (Buffer, Signature->signature.rsassa.sig.buffer, Signature->signature.rsassa.sig.size);
  Buffer += Signature->signature.rsassa.sig.size;

  SendBufferSize              = (UINT32)((UINTN)Buffer - (UINTN)&SendBuffer);
  SendBuffer.Header.paramSize = SwapBytes32 (SendBufferSize);

  //
  // send Tpm command
  //
  RecvBufferSize = sizeof (RecvBuffer);
  Status         = Tpm2SubmitCommand (SendBufferSize, (UINT8 *)&SendBuffer, &RecvBufferSize, (UINT8 *)&RecvBuffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (RecvBufferSize < sizeof (TPM2_RESPONSE_HEADER)) {
    DEBUG ((DEBUG_ERROR, "Tpm2VerifySignature - RecvBufferSize Error - %x\n", RecvBufferSize));
    return EFI_DEVICE_ERROR;
  }

  if (SwapBytes32 (RecvBuffer.Header.responseCode) != TPM_RC_SUCCESS) {
    // Includes TPM_RC_SIGNATURE — a well-formed but non-matching
    // signature. A tampered ROM or a forged/stale ticket lands here,
    // as an ordinary error return, not a crash.
    DEBUG ((DEBUG_ERROR, "Tpm2VerifySignature - responseCode - %x\n", SwapBytes32 (RecvBuffer.Header.responseCode)));
    return EFI_DEVICE_ERROR;
  }

  Validation->tag       = SwapBytes16 (ReadUnaligned16 ((UINT16 *)&RecvBuffer.Validation.tag));
  Validation->hierarchy = SwapBytes32 (ReadUnaligned32 ((UINT32 *)&RecvBuffer.Validation.hierarchy));

  TicketDigestSize = SwapBytes16 (ReadUnaligned16 ((UINT16 *)&RecvBuffer.Validation.digest.size));
  if (TicketDigestSize > sizeof (TPMU_HA)) {
    DEBUG ((DEBUG_ERROR, "Tpm2VerifySignature - ticket digest size error %x\n", TicketDigestSize));
    return EFI_DEVICE_ERROR;
  }

  Validation->digest.size = TicketDigestSize;
  CopyMem (Validation->digest.buffer, RecvBuffer.Validation.digest.buffer, TicketDigestSize);

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Tpm2PolicyAuthorize (
  IN TPMI_SH_POLICY     PolicySession,
  IN TPM2B_DIGEST       *ApprovedPolicy,
  IN TPM2B_NONCE        *PolicyRef,
  IN TPM2B_NAME         *KeySign,
  IN TPMT_TK_VERIFIED   *CheckTicket
  )
{
  EFI_STATUS                        Status;
  TPM2_POLICY_AUTHORIZE_COMMAND     SendBuffer;
  TPM2_POLICY_AUTHORIZE_RESPONSE    RecvBuffer;
  UINT32                            SendBufferSize;
  UINT32                            RecvBufferSize;
  UINT8                             *Buffer;

  //
  // Construct command
  //
  SendBuffer.Header.tag         = SwapBytes16 (TPM_ST_NO_SESSIONS);
  SendBuffer.Header.commandCode = SwapBytes32 (TPM_CC_PolicyAuthorize);

  SendBuffer.PolicySession = SwapBytes32 (PolicySession);

  Buffer = (UINT8 *)&SendBuffer.ApprovedPolicy;
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (ApprovedPolicy->size));
  Buffer += sizeof (UINT16);
  CopyMem (Buffer, ApprovedPolicy->buffer, ApprovedPolicy->size);
  Buffer += ApprovedPolicy->size;

  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (PolicyRef->size));
  Buffer += sizeof (UINT16);
  CopyMem (Buffer, PolicyRef->buffer, PolicyRef->size);
  Buffer += PolicyRef->size;

  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (KeySign->size));
  Buffer += sizeof (UINT16);
  CopyMem (Buffer, KeySign->name, KeySign->size);
  Buffer += KeySign->size;

  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (CheckTicket->tag));
  Buffer += sizeof (UINT16);
  WriteUnaligned32 ((UINT32 *)Buffer, SwapBytes32 (CheckTicket->hierarchy));
  Buffer += sizeof (UINT32);
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (CheckTicket->digest.size));
  Buffer += sizeof (UINT16);
  CopyMem (Buffer, CheckTicket->digest.buffer, CheckTicket->digest.size);
  Buffer += CheckTicket->digest.size;

  SendBufferSize              = (UINT32)((UINTN)Buffer - (UINTN)&SendBuffer);
  SendBuffer.Header.paramSize = SwapBytes32 (SendBufferSize);

  //
  // send Tpm command
  //
  RecvBufferSize = sizeof (RecvBuffer);
  Status         = Tpm2SubmitCommand (SendBufferSize, (UINT8 *)&SendBuffer, &RecvBufferSize, (UINT8 *)&RecvBuffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (RecvBufferSize < sizeof (TPM2_RESPONSE_HEADER)) {
    DEBUG ((DEBUG_ERROR, "Tpm2PolicyAuthorize - RecvBufferSize Error - %x\n", RecvBufferSize));
    return EFI_DEVICE_ERROR;
  }

  if (SwapBytes32 (RecvBuffer.Header.responseCode) != TPM_RC_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "Tpm2PolicyAuthorize - responseCode - %x\n", SwapBytes32 (RecvBuffer.Header.responseCode)));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}
