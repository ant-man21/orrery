/** @file
  Implements the PCR selection/session/extend helpers declared in
  Include/Library/Tpm2PcrLib.h. Factored out of TpmProvisionApp.c and
  TpmVerifyBootApp.c, which duplicated this byte-for-byte (see issue #8).
**/

#include <Library/Tpm2PcrLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/Tpm2CommandLib.h>
#include <Library/Tpm2DeviceLib.h>

VOID
EFIAPI
BuildPcrSelection (
  IN  UINT32              PcrIndex,
  OUT TPML_PCR_SELECTION  *Pcrs
  )
{
  ZeroMem (Pcrs, sizeof (*Pcrs));
  Pcrs->count                                   = 1;
  Pcrs->pcrSelections[0].hash                   = TPM_ALG_SHA256;
  Pcrs->pcrSelections[0].sizeofSelect            = 3;
  Pcrs->pcrSelections[0].pcrSelect[PcrIndex / 8] = (UINT8)(1 << (PcrIndex % 8));
}

EFI_STATUS
EFIAPI
OpenPcrPolicySession (
  IN  TPM_SE                SessionType,
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
    DEBUG ((DEBUG_ERROR, "OpenPcrPolicySession: Tpm2StartAuthSession failed: %r\n", Status));
  }

  return Status;
}

EFI_STATUS
EFIAPI
ExtendPcr (
  IN  EFI_TCG2_PROTOCOL  *Tcg2,
  IN  UINT32              PcrIndex,
  IN  VOID                *Data,
  IN  UINTN                DataSize,
  IN  CHAR8                *EventDescription,
  OUT TPML_DIGEST          *PcrValue OPTIONAL
  )
{
  EFI_STATUS      Status;
  EFI_TCG2_EVENT  *Event;
  UINTN           EventSize;
  UINTN           DescLen;

  DescLen   = AsciiStrLen (EventDescription);
  EventSize = sizeof (EFI_TCG2_EVENT) + DescLen;
  Event     = AllocateZeroPool (EventSize);
  if (Event == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Event->Size                 = (UINT32)EventSize;
  Event->Header.HeaderSize    = sizeof (EFI_TCG2_EVENT_HEADER);
  Event->Header.HeaderVersion = EFI_TCG2_EVENT_HEADER_VERSION;
  Event->Header.PCRIndex      = PcrIndex;
  Event->Header.EventType     = EV_POST_CODE;
  CopyMem (Event->Event, EventDescription, DescLen);

  Status = Tcg2->HashLogExtendEvent (
                   Tcg2,
                   0,                            /* Flags                  */
                   (EFI_PHYSICAL_ADDRESS)(UINTN)Data,
                   (UINT64)DataSize,
                   Event
                   );
  FreePool (Event);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ExtendPcr: HashLogExtendEvent failed: %r\n", Status));
    return Status;
  }

  if (PcrValue == NULL) {
    return EFI_SUCCESS;
  }

  {
    TPML_PCR_SELECTION  In;
    TPML_PCR_SELECTION  Out;
    UINT32              UpdateCounter;

    BuildPcrSelection (PcrIndex, &In);

    Status = Tpm2RequestUseTpm ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "ExtendPcr: Tpm2RequestUseTpm failed: %r\n", Status));
      return Status;
    }

    /* Tpm2PcrRead's PcrSelectionOut is never NULL-checked internally — a
     * real buffer is required even though we don't use its contents. */
    ZeroMem (&Out, sizeof (Out));
    ZeroMem (PcrValue, sizeof (*PcrValue));
    Status = Tpm2PcrRead (&In, &UpdateCounter, &Out, PcrValue);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "ExtendPcr: Tpm2PcrRead failed: %r\n", Status));
    }
  }

  return Status;
}
