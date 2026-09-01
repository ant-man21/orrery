/*
 * ep_info.c — populate entry_point_info_t register args for a BL32/BL33
 * handoff that carries a Transfer List address, per the AArch64 register
 * handoff convention (register_conventions.rst): X0=0, X1=signature+
 * version, X2=0, X3=Transfer List address. This exact 4-tuple is what
 * edk2's ValidateSpmMmBootInfo()/ValidateBootInfo()
 * (ArmPkg/Library/ArmStandaloneMmCoreEntryPoint/ArmStandaloneMmCoreEntryPoint.c)
 * checks for — see include/transfer_list.h's header comment.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdbool.h>
#include <stdint.h>

#include <transfer_list.h>

bool transfer_list_set_handoff_args(struct transfer_list_header *tl,
				     entry_point_info_t *ep_info)
{
	if ((tl == NULL) || (ep_info == NULL))
		return false;

	ep_info->args.arg0 = 0U;
	ep_info->args.arg1 = TRANSFER_LIST_HANDOFF_X1_VALUE(REGISTER_CONVENTION_VERSION);
	ep_info->args.arg2 = 0U;
	ep_info->args.arg3 = (uint64_t)(uintptr_t)tl;

	return true;
}
