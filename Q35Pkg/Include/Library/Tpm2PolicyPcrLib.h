/** @file
  Tpm2PolicyPCR — TPM2_PolicyPCR command wrapper.

  SecurityPkg's Tpm2CommandLib (edk2/SecurityPkg/Library/Tpm2CommandLib) wraps
  most TPM2 commands but not this one — EDK2's own firmware flows never
  needed a PCR-gated policy session, so nobody upstream wrote it.
**/

#ifndef TPM2_POLICY_PCR_LIB_H_
#define TPM2_POLICY_PCR_LIB_H_

#include <Uefi.h>
#include <IndustryStandard/Tpm20.h>

/**
  This command is used to cause conditional gating of a policy session based on the contents of the
  TPM PCR. If pcrDigest has a size of zero, then the digest of the selected PCR is used; otherwise,
  the value in pcrDigest is used (used for trial sessions to precompute a policy against a chosen
  PCR value).

  @param[in]  PolicySession      Handle for the policy session being extended.
  @param[in]  PcrDigest          Expected digest of the selected PCR using the session's hash algorithm.
                                  A size of zero tells the TPM to use the current value of the selected PCR.
  @param[in]  Pcrs               The PCR selection to be checked against pcrDigest.

  @retval EFI_SUCCESS            Operation completed successfully.
  @retval EFI_DEVICE_ERROR       The command was unsuccessful.
**/
EFI_STATUS
EFIAPI
Tpm2PolicyPCR (
  IN TPMI_SH_POLICY      PolicySession,
  IN TPM2B_DIGEST        *PcrDigest,
  IN TPML_PCR_SELECTION  *Pcrs
  );

#endif
