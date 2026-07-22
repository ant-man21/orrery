/** @file
  Common TPM2 PCR helpers shared between TpmProvisionApp and
  TpmVerifyBootApp: building a single-PCR TPML_PCR_SELECTION, opening a
  policy/trial session to fold PolicyPCR assertions into, and extending an
  arbitrary PCR via the TCG2 protocol.

  MODULE_TYPE is BASE (no UefiLib/Print dependency) so this stays usable
  from a future PEI-phase port (see anti_tamper_roadmap.md Phase 4) without
  a rewrite — callers are responsible for reporting failures on-screen.
**/

#ifndef TPM2_PCR_LIB_H_
#define TPM2_PCR_LIB_H_

#include <Uefi.h>
#include <Protocol/Tcg2Protocol.h>
#include <IndustryStandard/Tpm20.h>

/**
  Build a TPML_PCR_SELECTION selecting a single PCR on the SHA256 bank.

  @param[in]   PcrIndex   PCR to select (0-23).
  @param[out]  Pcrs       Selection structure to populate.
**/
VOID
EFIAPI
BuildPcrSelection (
  IN  UINT32              PcrIndex,
  OUT TPML_PCR_SELECTION  *Pcrs
  );

/**
  Open an unbound, unsalted session (trial or real policy) for folding
  TPM2_PolicyPCR assertions into.

  @param[in]   SessionType    TPM_SE_TRIAL or TPM_SE_POLICY.
  @param[out]  SessionHandle  Resulting session handle.

  @retval EFI_SUCCESS   Session opened.
  @retval other         Tpm2StartAuthSession failed; see DEBUG_ERROR log.
**/
EFI_STATUS
EFIAPI
OpenPcrPolicySession (
  IN  TPM_SE                SessionType,
  OUT TPMI_SH_AUTH_SESSION  *SessionHandle
  );

/**
  Extend the given PCR with the hash of Data via HashLogExtendEvent, then
  optionally read back the resulting PCR value so the caller can print it.

  @param[in]   Tcg2               TCG2 protocol instance.
  @param[in]   PcrIndex           PCR to extend (0-23).
  @param[in]   Data               Buffer to hash and extend into the PCR.
  @param[in]   DataSize           Size of Data in bytes.
  @param[in]   EventDescription   Short ASCII label for the event log entry
                                   (e.g. "BIOS ROM").
  @param[out]  PcrValue           OPTIONAL. If non-NULL, populated with the
                                   post-extend PCR digest via Tpm2PcrRead.

  @retval EFI_SUCCESS   Extend (and, if requested, read-back) succeeded.
  @retval other         HashLogExtendEvent, Tpm2RequestUseTpm, or
                         Tpm2PcrRead failed.
**/
EFI_STATUS
EFIAPI
ExtendPcr (
  IN  EFI_TCG2_PROTOCOL  *Tcg2,
  IN  UINT32              PcrIndex,
  IN  VOID                *Data,
  IN  UINTN                DataSize,
  IN  CHAR8                *EventDescription,
  OUT TPML_DIGEST          *PcrValue OPTIONAL
  );

#endif
