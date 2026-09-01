/*
 * logging.c — debug printing for the Transfer List, factored out of
 * transfer_list.c the same way upstream libtl separates it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>

#include <common/debug.h>
#include <transfer_list.h>

void transfer_list_log_entry(const struct transfer_list_entry *te, uint32_t offset)
{
	if (te == NULL)
		return;

	VERBOSE("transfer_list: entry @ offset 0x%x: tag_id=%u hdr_size=%u data_size=%u\n",
		offset, (unsigned int)te->tag_id, (unsigned int)te->hdr_size,
		(unsigned int)te->data_size);
}
