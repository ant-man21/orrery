/*
 * transfer_list.h — minimal "Firmware Handoff" Transfer List implementation.
 *
 * ORRERY: hand-written against the public Firmware Handoff specification
 * (https://github.com/FirmwareHandoff/firmware_handoff) because TF-A's own
 * reference implementation ("libtl", contrib/libtl in trusted-firmware-a)
 * is hosted exclusively at review.trustedfirmware.org — a Gerrit host this
 * build environment's network policy blocks outright, with no GitHub
 * mirror under any name we could find. See docs/sbsa_boot_flow.md for the
 * full story of why this is needed at all (qemu_sbsa's TF-A port and our
 * edk2 vintage's ArmStandaloneMmCoreEntryPoint.c only agree on the BL32
 * boot handoff format if TRANSFER_LIST=1/HOB_LIST=1 — confirmed against
 * edk2-platforms' own Platform/ARM/Readme.md, which documents exactly
 * this combination for Juno/FVP).
 *
 * The wire format below is not guessed: the struct layouts and tag ID
 * values are copied field-for-field from edk2's own
 * ArmPkg/Include/IndustryStandard/ArmTransferList.h — i.e. from the
 * actual consumer we need to interoperate with, not just the abstract
 * spec text. Only the scope is minimal: this implements exactly the
 * operations actually called by trusted-firmware-a/plat/qemu/common/
 * qemu_bl2_setup.c, qemu_bl31_setup.c, and
 * services/std_svc/spm/spm_mm/spm_mm_setup.c when built with
 * PLAT=qemu_sbsa SPM_MM=1 HOB_LIST=1 TRANSFER_LIST=1 — not the full
 * upstream libtl API surface (event log, dynamic config, OP-TEE pageable
 * part relocation, etc. used by other TF-A platforms this repo never
 * builds).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ORRERY_TRANSFER_LIST_H
#define ORRERY_TRANSFER_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <common/ep_info.h>

/* Transfer List (TL) header — 24 bytes, byte-for-byte identical to
 * edk2's TRANSFER_LIST_HEADER (ArmPkg/Include/IndustryStandard/
 * ArmTransferList.h). */
struct transfer_list_header {
	uint32_t signature;
	uint8_t  checksum;
	uint8_t  version;
	uint8_t  hdr_size;
	uint8_t  alignment;	/* log2 of the max alignment any TE needs */
	uint32_t used_size;
	uint32_t max_size;
	uint32_t flags;
	uint32_t reserved;
} __attribute__((packed, aligned(8)));

/* Transfer Entry (TE) header — 8 bytes, byte-for-byte identical to
 * edk2's TRANSFER_ENTRY_HEADER. Followed immediately by `data_size`
 * bytes of payload, then padding up to the next 8-byte boundary. */
struct transfer_list_entry {
	uint16_t tag_id;
	uint8_t  reserved0;
	uint8_t  hdr_size;
	uint32_t data_size;
} __attribute__((packed, aligned(8)));

#define TRANSFER_LIST_SIGNATURE		0x4a0fb10bU
#define TRANSFER_LIST_VERSION		0x01U
#define TRANSFER_LIST_FL_HAS_CHECKSUM	(1U << 0)

/* Register handoff convention (register_conventions.rst): the receiver
 * checks the low 32 bits of X1 against TRANSFER_LIST_SIGNATURE and bits
 * [39:32] against this version. Matches edk2's REGISTER_CONVENTION_VERSION
 * and CREATE_TRANSFER_LIST_HANDOFF_X1_VALUE() exactly. */
#define REGISTER_CONVENTION_VERSION		1U
#define REGISTER_CONVENTION_VERSION_SHIFT_64	32U
#define REGISTER_CONVENTION_VERSION_SHIFT_32	24U

#define TRANSFER_LIST_HANDOFF_X1_VALUE(version) \
	(((uint64_t)TRANSFER_LIST_SIGNATURE) | \
	 ((uint64_t)(version) << REGISTER_CONVENTION_VERSION_SHIFT_64))

/* AArch32 handoff uses a shorter 20-bit signature to leave room for an
 * 8-bit version within a single 32-bit register — not exercised on this
 * (AArch64-only) platform, but referenced unconditionally by
 * qemu_bl2_setup.c's is64/else branch, so it must still compile. */
#define TRANSFER_LIST_SIGNATURE_32		0xfb10bU
#define TRANSFER_LIST_HANDOFF_R1_VALUE(version) \
	(((uint32_t)TRANSFER_LIST_SIGNATURE_32) | \
	 ((uint32_t)(version) << REGISTER_CONVENTION_VERSION_SHIFT_32))

/* Standard tag IDs. TL_TAG_HOB_LIST's value (3) MUST match edk2's
 * TRANSFER_ENTRY_TAG_ID_HOB_LIST — that's the one tag our actual boot
 * path (spm_mm_setup.c) both writes and edk2 reads. The others are
 * referenced (by name) from qemu_bl2_setup.c for images/manifests this
 * platform never actually loads with SPM_MM=1 alone; their values only
 * need to be distinct, not standards-compliant, since nothing on the
 * edk2 side reads them in this configuration. */
#define TL_TAG_EMPTY			0U
#define TL_TAG_FDT			1U
#define TL_TAG_HOB			2U
#define TL_TAG_HOB_LIST			3U	/* must match edk2 exactly */
#define TL_TAG_ACPI_TABLE_AGGREGATE	4U
#define TL_TAG_TPM_EVLOG		5U
#define TL_TAG_TPM_CRB_BASE		6U
#define TL_TAG_DT_FFA_MANIFEST		7U
#define TL_TAG_DT_SPMC_MANIFEST		8U
#define TL_TAG_OPTEE_PAGABLE_PART	9U

/* --- generic/transfer_list.c --- */

struct transfer_list_header *transfer_list_init(void *addr, size_t max_size);

struct transfer_list_header *transfer_list_check_header(const void *addr);

void transfer_list_update_checksum(struct transfer_list_header *tl);

struct transfer_list_entry *transfer_list_add(struct transfer_list_header *tl,
					       uint32_t tag_id,
					       uint32_t data_size,
					       const void *data);

void *transfer_list_entry_data(struct transfer_list_entry *entry);

struct transfer_list_header *transfer_list_relocate(struct transfer_list_header *tl,
						      void *new_addr,
						      size_t new_max_size);

void transfer_list_dump(struct transfer_list_header *tl);

/* --- arm/ep_info.c --- */

bool transfer_list_set_handoff_args(struct transfer_list_header *tl,
				     entry_point_info_t *ep_info);

#endif /* ORRERY_TRANSFER_LIST_H */
