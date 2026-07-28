/** @file
  Tpm2LoadExternal / Tpm2VerifySignature / Tpm2PolicyAuthorize — TPM2
  command wrappers for the signed-ticket reprovisioning design (issue #15).

  SecurityPkg's Tpm2CommandLib (edk2/SecurityPkg/Library/Tpm2CommandLib)
  doesn't wrap any of these three — same gap as Tpm2PolicyPCR
  (Tpm2PolicyPcrLib), same reason: EDK2's own firmware flows never needed
  an externally-signed, key-authorized policy, so nobody upstream wrote it.

  This library only supports the narrow shapes this project actually
  uses — RSA-2048 verification-only keys (see TrustedUpdateKey.h), RSASSA
  signatures, SHA-256 throughout. It is not a general-purpose TPM2 object/
  signature marshaller.
**/

#ifndef TPM2_POLICY_AUTHORIZE_LIB_H_
#define TPM2_POLICY_AUTHORIZE_LIB_H_

#include <Uefi.h>
#include <IndustryStandard/Tpm20.h>

#ifndef TPM_ALG_RSA
//
// This fork's MdePkg/Include/IndustryStandard/Tpm20.h has TPM_ALG_RSA
// commented out (name collides with a differently-sized TPM 1.2 constant
// of the same name in Tpm12.h — see the comment there). Confirmed by
// checking SecurityPkg/Library/Tpm2CommandLib/Tpm2Object.c, which
// references bare `TPM_ALG_RSA` as though it resolved — it doesn't in
// this fork, safely, for our purposes. Defining it locally rather than
// depending on whatever that file is actually pulling in.
//
#define TPM_ALG_RSA  ((TPM_ALG_ID)0x0001)
#endif

/**
  TPM2_LoadExternal — load a public-only external object (no Sensitive
  area, hierarchy = TPM_RH_NULL) just long enough to reference its Name.
  Only supports the RSA public-key shape TrustedUpdateKey.h produces.

  @param[in]  InPublic       The public area to load (type must be
                               TPM_ALG_RSA).
  @param[out] ObjectHandle   Handle for the loaded object. Caller must
                               Tpm2FlushContext() it when done.
  @param[out] Name           The object's Name (hash of its public area) —
                               what Tpm2NvDefineSpace's authPolicy is
                               permanently keyed to.

  @retval EFI_SUCCESS        Operation completed successfully.
  @retval EFI_DEVICE_ERROR   The command was unsuccessful.
**/
EFI_STATUS
EFIAPI
Tpm2LoadExternal (
  IN  TPM2B_PUBLIC  *InPublic,
  OUT TPM_HANDLE    *ObjectHandle,
  OUT TPM2B_NAME    *Name
  );

/**
  TPM2_VerifySignature — check whether Signature is a genuine signature
  over Digest, made by the key loaded at KeyHandle. On success, the TPM
  returns a ticket proving this check happened — proof the TPM itself
  did the check, consumed by Tpm2PolicyAuthorize, not proof by itself of
  anything about the policy session. Only supports TPM_ALG_RSASSA
  signatures (what sign_rom_ticket.py produces).

  @param[in]  KeyHandle      Handle of the loaded public key (see
                               Tpm2LoadExternal).
  @param[in]  Digest         The digest that was signed.
  @param[in]  Signature      The signature to check (sigAlg must be
                               TPM_ALG_RSASSA).
  @param[out] Validation     Ticket proving the signature checked out.

  @retval EFI_SUCCESS        Signature verified.
  @retval EFI_DEVICE_ERROR   The command was unsuccessful, or the
                               signature did not verify (TPM_RC_SIGNATURE)
                               — a tampered ROM or a forged/stale ticket
                               both land here, not as a crash.
**/
EFI_STATUS
EFIAPI
Tpm2VerifySignature (
  IN  TPM_HANDLE         KeyHandle,
  IN  TPM2B_DIGEST       *Digest,
  IN  TPMT_SIGNATURE     *Signature,
  OUT TPMT_TK_VERIFIED   *Validation
  );

/**
  TPM2_PolicyAuthorize — given a verification ticket, discard the policy
  session's running digest (e.g. whatever Tpm2PolicyPCR just produced for
  the live ROM) and replace it with a fixed digest that depends only on
  KeySign's Name. This substitution is the entire mechanism: the vault's
  authPolicy (set once, at N=0, via a trial session that calls this same
  command with an empty ticket) is exactly what this call always produces
  for a given signing key — regardless of what ApprovedPolicy was. A new
  firmware update never needs the vault's rule to change; only the
  ticket does.

  @param[in]  PolicySession    The policy session to authorize.
  @param[in]  ApprovedPolicy   The policy digest the ticket was computed
                                 over — this session's digest immediately
                                 before this call (e.g. from
                                 Tpm2PolicyGetDigest right after
                                 Tpm2PolicyPCR). Size 0 for the N=0 trial
                                 session that has no prior PolicyPCR step.
  @param[in]  PolicyRef        Additional signed context. This project
                                 always uses an empty (size 0) one — pass
                                 a zeroed TPM2B_NONCE, not NULL, matching
                                 Tpm2PolicyPCR's own EmptyPcrDigest
                                 convention.
  @param[in]  KeySign          Name of the key that produced the ticket.
  @param[in]  CheckTicket      Ticket from Tpm2VerifySignature (or a
                                 zeroed TPMT_TK_VERIFIED with
                                 hierarchy = TPM_RH_NULL for a trial
                                 session, which skips ticket validation
                                 entirely).

  @retval EFI_SUCCESS          Operation completed successfully.
  @retval EFI_DEVICE_ERROR     The command was unsuccessful.
**/
EFI_STATUS
EFIAPI
Tpm2PolicyAuthorize (
  IN TPMI_SH_POLICY     PolicySession,
  IN TPM2B_DIGEST       *ApprovedPolicy,
  IN TPM2B_NONCE        *PolicyRef,
  IN TPM2B_NAME         *KeySign,
  IN TPMT_TK_VERIFIED   *CheckTicket
  );

#endif
