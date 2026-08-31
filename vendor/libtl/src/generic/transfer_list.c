/*
 * transfer_list.c — core Transfer List operations (init/add/lookup/
 * checksum/relocate). See include/transfer_list.h for provenance notes.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <string.h>

#include <transfer_list.h>

#define TL_ALIGN 8U

/* Declared in logging.c; kept out of the public header since it's an
 * internal formatting helper, not part of the API surface TF-A calls. */
void transfer_list_log_entry(const struct transfer_list_entry *te, uint32_t offset);

static size_t align_up(size_t value, size_t alignment)
{
	return (value + (alignment - 1U)) & ~(alignment - 1U);
}

/* Sums every byte in [tl, tl + used_size) exactly as it stands — no
 * assumption about what the checksum field currently holds. Used two
 * ways: during generation, the caller zeroes tl->checksum first, so the
 * result is "what would the total be with checksum contributing 0";
 * during verification, the caller leaves the stored checksum in place,
 * so the result is the real total, which a valid TL must sum to 0. */
static uint8_t sum_all_bytes(const struct transfer_list_header *tl)
{
	const uint8_t *bytes = (const uint8_t *)tl;
	uint8_t sum = 0U;
	uint32_t i;

	for (i = 0U; i < tl->used_size; i++)
		sum = (uint8_t)(sum + bytes[i]);

	return sum;
}

void transfer_list_update_checksum(struct transfer_list_header *tl)
{
	uint8_t sum;

	if (tl == NULL)
		return;

	tl->checksum = 0U;
	sum = sum_all_bytes(tl);
	/* Sum over every byte (this one included) must be 0 mod 256. */
	tl->checksum = (uint8_t)(0U - sum);
}

struct transfer_list_header *transfer_list_init(void *addr, size_t max_size)
{
	struct transfer_list_header *tl;

	if ((addr == NULL) || (max_size < sizeof(struct transfer_list_header)))
		return NULL;

	if (((uintptr_t)addr % TL_ALIGN) != 0U)
		return NULL;

	tl = (struct transfer_list_header *)addr;
	memset(tl, 0, sizeof(*tl));

	tl->signature  = TRANSFER_LIST_SIGNATURE;
	tl->version    = TRANSFER_LIST_VERSION;
	tl->hdr_size   = (uint8_t)sizeof(*tl);
	tl->alignment  = 3U; /* 2^3 = 8-byte alignment */
	tl->used_size  = (uint32_t)align_up(sizeof(*tl), TL_ALIGN);
	tl->max_size = (uint32_t)max_size;
	tl->flags      = TRANSFER_LIST_FL_HAS_CHECKSUM;

	transfer_list_update_checksum(tl);

	return tl;
}

struct transfer_list_header *transfer_list_check_header(const void *addr)
{
	const struct transfer_list_header *tl = addr;

	if (addr == NULL)
		return NULL;

	if (((uintptr_t)addr % TL_ALIGN) != 0U)
		return NULL;

	if (tl->signature != TRANSFER_LIST_SIGNATURE)
		return NULL;

	if ((tl->hdr_size < sizeof(struct transfer_list_header)) ||
	    (tl->used_size > tl->max_size) ||
	    (tl->used_size < tl->hdr_size)) {
		return NULL;
	}

	if ((tl->flags & TRANSFER_LIST_FL_HAS_CHECKSUM) != 0U) {
		/* A valid TL's stored checksum makes the sum of every byte,
		 * checksum byte included as-is, equal to 0 mod 256. */
		if (sum_all_bytes(tl) != 0U)
			return NULL;
	}

	return (struct transfer_list_header *)(uintptr_t)addr;
}

struct transfer_list_entry *transfer_list_add(struct transfer_list_header *tl,
					       uint32_t tag_id,
					       uint32_t data_size,
					       const void *data)
{
	struct transfer_list_entry *te;
	uint32_t entry_offset;
	uint32_t new_used_size;

	if (tl == NULL)
		return NULL;

	entry_offset = tl->used_size;
	new_used_size = (uint32_t)align_up(
		(size_t)entry_offset + sizeof(struct transfer_list_entry) + data_size,
		TL_ALIGN);

	if ((new_used_size > tl->max_size) || (new_used_size < entry_offset))
		return NULL; /* out of space, or overflow */

	te = (struct transfer_list_entry *)((uint8_t *)tl + entry_offset);
	memset(te, 0, sizeof(*te));
	te->tag_id    = (uint16_t)tag_id;
	te->hdr_size  = (uint8_t)sizeof(*te);
	te->data_size = data_size;

	if (data_size > 0U) {
		if (data != NULL)
			memcpy((uint8_t *)te + te->hdr_size, data, data_size);
		else
			memset((uint8_t *)te + te->hdr_size, 0, data_size);
	}

	tl->used_size = new_used_size;
	transfer_list_update_checksum(tl);

	transfer_list_log_entry(te, entry_offset);

	return te;
}

void *transfer_list_entry_data(struct transfer_list_entry *entry)
{
	if (entry == NULL)
		return NULL;

	return (uint8_t *)entry + entry->hdr_size;
}

struct transfer_list_header *transfer_list_relocate(struct transfer_list_header *tl,
						      void *new_addr,
						      size_t new_max_size)
{
	struct transfer_list_header *new_tl;

	if ((tl == NULL) || (new_addr == NULL) || (new_max_size < tl->used_size))
		return NULL;

	if (((uintptr_t)new_addr % TL_ALIGN) != 0U)
		return NULL;

	memmove(new_addr, tl, tl->used_size);

	new_tl = (struct transfer_list_header *)new_addr;
	new_tl->max_size = (uint32_t)new_max_size;
	transfer_list_update_checksum(new_tl);

	return new_tl;
}

void transfer_list_dump(struct transfer_list_header *tl)
{
	uint32_t offset;

	if (tl == NULL)
		return;

	offset = tl->hdr_size;
	while (offset < tl->used_size) {
		struct transfer_list_entry *te =
			(struct transfer_list_entry *)((uint8_t *)tl + offset);

		if (te->hdr_size < sizeof(struct transfer_list_entry))
			break; /* corrupt entry, stop walking */

		transfer_list_log_entry(te, offset);

		offset += (uint32_t)align_up((size_t)te->hdr_size + te->data_size, TL_ALIGN);
	}
}
