/** @file 

Hello DXE driver

**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/PcdLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>



/**
  Main entry for HelloDxe driver.

  @param ImageHandle     Image handle for HelloDxe driver.
  @param SystemTable     Pointer to SystemTable.

  @return Status of gBS->InstallProtocolInterface()

**/
EFI_STATUS
EFIAPI
HelloDxeEntry (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE  *SystemTable
)
{
    EFI_STATUS  Status;
    // VOID        *Registration;

    DEBUG((DEBUG_ERROR , "HelloDxeEntry start...\n"));
    DEBUG((DEBUG_ERROR , "HELLO QUACKMEAT\n"));
    Status = EFI_SUCCESS;
    
    DEBUG((DEBUG_INFO, "HelloDxeEntry end...\n"));

    return Status;
}