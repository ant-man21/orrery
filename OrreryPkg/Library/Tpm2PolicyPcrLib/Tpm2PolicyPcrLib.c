/** @file
  Implement the TPM2_PolicyPCR command (see Include/Library/Tpm2PolicyPcrLib.h
  for why this lives here instead of edk2/SecurityPkg/Library/Tpm2CommandLib).
**/

#include <Library/Tpm2PolicyPcrLib.h>
#include <Library/Tpm2DeviceLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>

#pragma pack(1)

typedef struct {
  TPM2_COMMAND_HEADER    Header;
  TPMI_SH_POLICY         PolicySession;
  TPM2B_DIGEST           PcrDigest;
  TPML_PCR_SELECTION     Pcrs;
} TPM2_POLICY_PCR_COMMAND;

typedef struct {
  TPM2_RESPONSE_HEADER    Header;
} TPM2_POLICY_PCR_RESPONSE;

#pragma pack()

EFI_STATUS
EFIAPI
Tpm2PolicyPCR (
  IN TPMI_SH_POLICY      PolicySession,
  IN TPM2B_DIGEST        *PcrDigest,
  IN TPML_PCR_SELECTION  *Pcrs
  )
{
  EFI_STATUS                Status;
  TPM2_POLICY_PCR_COMMAND   SendBuffer;
  TPM2_POLICY_PCR_RESPONSE  RecvBuffer;
  UINT32                    SendBufferSize;
  UINT32                    RecvBufferSize;
  UINT8                     *Buffer;
  UINTN                     Index;

  //
  // Construct command
  //
  SendBuffer.Header.tag         = SwapBytes16 (TPM_ST_NO_SESSIONS);
  SendBuffer.Header.commandCode = SwapBytes32 (TPM_CC_PolicyPCR);

  SendBuffer.PolicySession = SwapBytes32 (PolicySession);

  Buffer = (UINT8 *)&SendBuffer.PcrDigest;
  WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (PcrDigest->size));
  Buffer += sizeof (UINT16);
  CopyMem (Buffer, PcrDigest->buffer, PcrDigest->size);
  Buffer += PcrDigest->size;

  WriteUnaligned32 ((UINT32 *)Buffer, SwapBytes32 (Pcrs->count));
  Buffer += sizeof (UINT32);
  for (Index = 0; Index < Pcrs->count; Index++) {
    WriteUnaligned16 ((UINT16 *)Buffer, SwapBytes16 (Pcrs->pcrSelections[Index].hash));
    Buffer += sizeof (UINT16);
    *(UINT8 *)Buffer = Pcrs->pcrSelections[Index].sizeofSelect;
    Buffer++;
    CopyMem (Buffer, &Pcrs->pcrSelections[Index].pcrSelect, Pcrs->pcrSelections[Index].sizeofSelect);
    Buffer += Pcrs->pcrSelections[Index].sizeofSelect;
  }

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
    DEBUG ((DEBUG_ERROR, "Tpm2PolicyPCR - RecvBufferSize Error - %x\n", RecvBufferSize));
    return EFI_DEVICE_ERROR;
  }

  if (SwapBytes32 (RecvBuffer.Header.responseCode) != TPM_RC_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "Tpm2PolicyPCR - responseCode - %x\n", SwapBytes32 (RecvBuffer.Header.responseCode)));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}
