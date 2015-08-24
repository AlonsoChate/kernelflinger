/*
 * Copyright (c) 2015, Intel Corporation
 * All rights reserved.
 *
 * Authors: Jeremy Compostella <jeremy.compostella@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <lib.h>

#include "reader.h"
#include "acpi.h"
#include "sparse_format.h"

/* RAM reader avoid dynamic memory allocation to avoid RAM corruption
   during the dump.  */
#define MAX_MEMORY_REGION_NB 256

static struct ram_priv {
	BOOLEAN is_in_used;

	/* Memory map */
	UINT8 memmap[MAX_MEMORY_REGION_NB * sizeof(EFI_MEMORY_DESCRIPTOR)];

	/* Boundaries */
	EFI_PHYSICAL_ADDRESS start;
	EFI_PHYSICAL_ADDRESS end;

	/* Current memory region */
	EFI_PHYSICAL_ADDRESS cur;
	EFI_PHYSICAL_ADDRESS cur_end;

	/* Sparse format */
	UINTN chunk_nb;
	UINTN cur_chunk;
	struct sparse_header sheader;
	struct chunk_header chunks[MAX_MEMORY_REGION_NB];
} ram_priv;

static VOID sort_memory_map(CHAR8 *entries, UINTN nr_entries, UINTN entry_sz)
{
	BOOLEAN swapped;
	EFI_MEMORY_DESCRIPTOR *cur, *next;
	UINTN i;

	/* Bubble sort algorithm */
	do {
		swapped = FALSE;
		for (i = 0; i < nr_entries - 1; i++) {
			cur = (EFI_MEMORY_DESCRIPTOR *)(entries + entry_sz * i);
			next = (EFI_MEMORY_DESCRIPTOR *)(entries + entry_sz * (i + 1));
			if (cur->PhysicalStart > next->PhysicalStart) {
				CHAR8 save[entry_sz];
				memcpy(save, cur, entry_sz);
				memcpy(cur, next, entry_sz);
				memcpy(next, save, entry_sz);
				swapped = TRUE;
			}
		}
		nr_entries--;
	} while (swapped);
}

static EFI_STATUS ram_add_chunk(reader_ctx_t *ctx, struct ram_priv *priv, UINT16 type, UINT64 size)
{
	struct chunk_header *cur = NULL;

	if (size % EFI_PAGE_SIZE) {
		error(L"chunk size must be multiple of %d bytes", EFI_PAGE_SIZE);
		return EFI_INVALID_PARAMETER;
	}

	if (priv->chunk_nb == MAX_MEMORY_REGION_NB) {
		error(L"Failed to allocate a new chunk");
		return EFI_OUT_OF_RESOURCES;
	}

	cur = &priv->chunks[priv->chunk_nb++];

	cur->chunk_type = type;
	cur->chunk_sz = size / EFI_PAGE_SIZE;
	cur->total_sz = sizeof(*cur);
	ctx->len += sizeof(*cur);
	if (type == CHUNK_TYPE_RAW) {
		cur->total_sz += size;
		ctx->len += size;
	}

	priv->sheader.total_chunks++;
	priv->sheader.total_blks += cur->chunk_sz;

	return EFI_SUCCESS;
}

static EFI_STATUS ram_build_chunks(reader_ctx_t *ctx, struct ram_priv *priv,
				   UINTN nr_entries, UINTN entry_sz)
{
	EFI_STATUS ret = EFI_SUCCESS;
	UINT16 type;
	UINTN i;
	EFI_MEMORY_DESCRIPTOR *entry;
	UINT64 entry_len, length;
	EFI_PHYSICAL_ADDRESS entry_end, prev_end;
	CHAR8 *entries = priv->memmap;

	prev_end = ctx->cur = ctx->len = 0;

	for (i = 0; i < nr_entries; entries += entry_sz, i++) {
		entry = (EFI_MEMORY_DESCRIPTOR *)entries;
		entry_len = entry->NumberOfPages * EFI_PAGE_SIZE;
		entry_end = entry->PhysicalStart + entry_len;

		if (priv->start >= entry_end)
			goto next;

		/* Memory hole between two memory regions */
		if (prev_end != entry->PhysicalStart) {
			if (prev_end > entry->PhysicalStart) {
				error(L"overlap detected, aborting");
				goto err;
			}

			length = entry->PhysicalStart - prev_end;

			if (priv->start > prev_end && priv->start < entry->PhysicalStart)
				length -= priv->start - prev_end;

			if (priv->end && entry->PhysicalStart > priv->end)
				length -= entry->PhysicalStart - priv->end;

			ret = ram_add_chunk(ctx, priv, CHUNK_TYPE_DONT_CARE, length);
			if (EFI_ERROR(ret))
				goto err;

			if (priv->end && priv->end < entry->PhysicalStart)
				break;
		}

		length = entry_len;
		if (priv->start > entry->PhysicalStart && priv->start < entry_end)
			length -= priv->start - entry->PhysicalStart;

		if (priv->end && priv->end < entry_end)
			length -= entry_end - priv->end;

		type = entry->Type == EfiConventionalMemory ? CHUNK_TYPE_RAW : CHUNK_TYPE_DONT_CARE;
		ret = ram_add_chunk(ctx, priv, type, length);
		if (EFI_ERROR(ret))
			goto err;

		if (priv->end && priv->end <= entry_end)
			break;

next:
		prev_end = entry_end;
	}

	if (priv->end && i == nr_entries) {
		error(L"End boundary is in unreachable memory region (>= 0x%lx)",
		      prev_end);
		return EFI_INVALID_PARAMETER;
	}

	if (!ctx->len) {
		error(L"Start boundary is in unreachable memory region");
		return EFI_INVALID_PARAMETER;
	}

	if (!priv->end)
		priv->end = prev_end;

	return EFI_SUCCESS;

err:
	if (priv->chunks)
		FreePool(priv->chunks);

	return EFI_ERROR(ret) ? ret : EFI_INVALID_PARAMETER;
}

static EFI_STATUS ram_open(reader_ctx_t *ctx, UINTN argc, char **argv)
{
	EFI_STATUS ret = EFI_SUCCESS;
	struct ram_priv *priv;
	char *endptr;
	CHAR8 *entries = NULL;
	UINT32 descr_ver;
	UINTN descr_sz, key, memmap_sz, nr_descr;
	UINT64 length;

	if (argc > 2)
		return EFI_INVALID_PARAMETER;

	if (ram_priv.is_in_used)
		return EFI_UNSUPPORTED;

	ctx->private = priv = &ram_priv;
	memset(priv, 0, sizeof(*priv));
	priv->is_in_used = TRUE;

	/* Parse argv  */
	if (argc > 0) {
		priv->start = strtoul(argv[0], &endptr, 16);
		if (*endptr != '\0')
			goto err;
	}

	if (argc == 2) {
		length = strtoul(argv[1], &endptr, 16);
		if (*endptr != '\0')
			goto err;
		priv->end = priv->start + length;
	} else
		priv->end = 0;

	if (priv->start % EFI_PAGE_SIZE || priv->end % EFI_PAGE_SIZE) {
		error(L"Boundaries must be multiple of %d bytes", EFI_PAGE_SIZE);
		goto err;
	}

	/* Initialize sparse header */
	priv->sheader.magic = SPARSE_HEADER_MAGIC;
	priv->sheader.major_version = 0x1;
	priv->sheader.minor_version = 0;
	priv->sheader.file_hdr_sz = sizeof(priv->sheader);
	priv->sheader.chunk_hdr_sz = sizeof(*priv->chunks);
	priv->sheader.blk_sz = EFI_PAGE_SIZE;

	memmap_sz = sizeof(priv->memmap);
	ret = uefi_call_wrapper(BS->GetMemoryMap, 5, &memmap_sz,
				(EFI_MEMORY_DESCRIPTOR *)priv->memmap,
				&key, &descr_sz, &descr_ver);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to get the current memory map");
		goto err;
	}
	nr_descr = memmap_sz / descr_sz;
	sort_memory_map(priv->memmap, nr_descr, descr_sz);

	ret = ram_build_chunks(ctx, priv, nr_descr, descr_sz);
	FreePool(entries);
	if (EFI_ERROR(ret))
		goto err;

	ctx->len += sizeof(priv->sheader);

	return EFI_SUCCESS;

err:
	priv->is_in_used = FALSE;
	return EFI_ERROR(ret) ? ret : EFI_INVALID_PARAMETER;
}

static EFI_STATUS ram_read(reader_ctx_t *ctx, unsigned char **buf, UINTN *len)
{
	struct ram_priv *priv = ctx->private;
	struct chunk_header *chunk;

	/* First byte, send the sparse header */
	if (ctx->cur == 0) {
		if (*len < sizeof(priv->sheader))
			return EFI_INVALID_PARAMETER;

		*buf = (unsigned char *)&priv->sheader;
		*len = sizeof(priv->sheader);
		priv->cur = priv->cur_end = priv->start;
		return EFI_SUCCESS;
	}

	/* Start new chunk */
	if (priv->cur == priv->cur_end) {
		if (priv->cur_chunk == priv->chunk_nb || *len < sizeof(*priv->chunks))
			return EFI_INVALID_PARAMETER;

		chunk = &priv->chunks[priv->cur_chunk++];
		*buf = (unsigned char *)chunk;
		*len = sizeof(*chunk);
		priv->cur_end = priv->cur + chunk->chunk_sz * EFI_PAGE_SIZE;
		if (chunk->chunk_type != CHUNK_TYPE_RAW)
			priv->cur = priv->cur_end;
		return EFI_SUCCESS;
	}

	/* Continue to send the current memory region */
	*len = min(*len, priv->cur_end - priv->cur);
	*buf = (unsigned char *)priv->cur;
	priv->cur += *len;

	return EFI_SUCCESS;
}

static void ram_close(reader_ctx_t *ctx)
{
	struct ram_priv *priv = ctx->private;

	FreePool(priv->chunks);
	priv->is_in_used = FALSE;
}

/* Partition reader */
#define PART_READER_BUF_SIZE (10 * 1024 * 1024)

struct part_priv {
	struct gpt_partition_interface gparti;
	BOOLEAN need_more_data;
	unsigned char buf[PART_READER_BUF_SIZE];
	UINTN buf_cur;
	UINTN buf_len;
	UINT64 offset;
};

static EFI_STATUS part_open(reader_ctx_t *ctx, UINTN argc, char **argv)
{
	EFI_STATUS ret = EFI_SUCCESS;
	struct gpt_partition_interface *gparti;
	struct part_priv *priv;
	CHAR16 *partname;
	UINT64 length;

	if (argc < 1 || argc > 3)
		return EFI_INVALID_PARAMETER;

	priv = ctx->private = AllocatePool(sizeof(*priv));
	if (!priv)
		return EFI_OUT_OF_RESOURCES;


	partname = stra_to_str((CHAR8 *)argv[0]);
	if (!partname) {
		error(L"Failed to convert partition name to CHAR16");
		goto err;
	}

	gparti = &priv->gparti;
	ret = gpt_get_partition_by_label(partname, gparti, LOGICAL_UNIT_USER);
	FreePool(partname);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Cannot access partition '%a'", argv[0]);
		goto err;
	}

	priv->offset = gparti->part.starting_lba * gparti->bio->Media->BlockSize;
	length = (gparti->part.ending_lba + 1 - gparti->part.starting_lba) *
		gparti->bio->Media->BlockSize;

	ctx->cur = 0;
	ctx->len = length;

	if (argc > 1) {
		ctx->cur = strtoul(argv[1], NULL, 16);
		if (ctx->cur >= length)
			goto err;
	}

	if (argc == 3) {
		ctx->len = strtoul(argv[2], NULL, 16);
		if (ctx->len == 0 || ctx->len > length || ctx->cur >= length - ctx->len)
			goto err;
	}

	priv->buf_cur = 0;
	priv->buf_len = 0;
	priv->need_more_data = TRUE;

	return EFI_SUCCESS;

err:
	FreePool(priv);
	return EFI_ERROR(ret) ? ret : EFI_INVALID_PARAMETER;
}

static EFI_STATUS part_read(reader_ctx_t *ctx, unsigned char **buf, UINTN *len)
{
	EFI_STATUS ret;
	struct part_priv *priv = ctx->private;

	if (priv->need_more_data) {
		priv->buf_len = min(sizeof(priv->buf), ctx->len - ctx->cur);
		ret = uefi_call_wrapper(priv->gparti.dio->ReadDisk, 5, priv->gparti.dio,
					priv->gparti.bio->Media->MediaId,
					priv->offset + ctx->cur, priv->buf_len, priv->buf);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to read partition");
			return ret;
		}

		priv->need_more_data = FALSE;
		priv->buf_cur = 0;
	}

	*len = min(*len, priv->buf_len - priv->buf_cur);
	*buf = priv->buf + priv->buf_cur;
	priv->buf_cur += *len;
	if (priv->buf_cur == priv->buf_len)
		priv->need_more_data = TRUE;

	return EFI_SUCCESS;
}

/* ACPI table reader */
static EFI_STATUS acpi_open(reader_ctx_t *ctx, UINTN argc, char **argv)
{
	EFI_STATUS ret;
	struct ACPI_DESC_HEADER *table;

	if (argc != 1)
		return EFI_INVALID_PARAMETER;

	ret = get_acpi_table((CHAR8 *)argv[0], (VOID **)&table);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Cannot access ACPI table %a", argv[0]);
		return ret;
	}

	ctx->private = table;
	ctx->cur = 0;
	ctx->len = table->length;

	return EFI_SUCCESS;
}

/* EFI variable reader */
static EFI_STATUS efivar_find(CHAR16 *varname, EFI_GUID *guid_p)
{
	EFI_STATUS ret;
	UINTN bufsize, namesize;
	CHAR16 *name;
	EFI_GUID guid;
	BOOLEAN found = FALSE;
	EFI_GUID found_guid;

	bufsize = 64;		/* Initial size large enough to handle
				   usual variable names length and
				   avoid the ReallocatePool as much as
				   possible.  */
	name = AllocateZeroPool(bufsize);
	if (!name) {
		error(L"Failed to re-allocate variable name buffer");
		return EFI_OUT_OF_RESOURCES;
	}

	for (;;) {
		namesize = bufsize;
		ret = uefi_call_wrapper(RT->GetNextVariableName, 3, &namesize,
					name, &guid);
		if (ret == EFI_NOT_FOUND) {
			ret = EFI_SUCCESS;
			break;
		}
		if (ret == EFI_BUFFER_TOO_SMALL) {
			name = ReallocatePool(name, bufsize, namesize);
			if (!name) {
				error(L"Failed to re-allocate variable name buffer");
				return EFI_OUT_OF_RESOURCES;
			}
			bufsize = namesize;
			continue;
		}
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"GetNextVariableName failed");
			break;
		}

		if (!StrCmp(name, varname)) {
			if (found) {
				error(L"Found 2 variables named %s", varname);
				ret = EFI_UNSUPPORTED;
				break;
			}
			found = TRUE;
			found_guid = guid;
		}
	}

	FreePool(name);

	if (EFI_ERROR(ret))
		return ret;

	if (!found)
		return EFI_NOT_FOUND;

	*guid_p = found_guid;
	return EFI_SUCCESS;
}

static EFI_STATUS efivar_open(reader_ctx_t *ctx, UINTN argc, char **argv)
{
	EFI_STATUS ret;
	UINT32 flags;
	UINTN size;
	CHAR16 *varname = NULL;
	EFI_GUID guid;

	if (argc != 1 && argc != 2)
		return EFI_INVALID_PARAMETER;

	if (argc == 2) {
		ret = stra_to_guid(argv[1], &guid);
		if (EFI_ERROR(ret))
			return ret;
	}

	varname = stra_to_str((CHAR8 *)argv[0]);
	if (!varname)
		return EFI_OUT_OF_RESOURCES;

	if (argc == 1) {
		ret = efivar_find(varname, &guid);
		if (EFI_ERROR(ret))
			goto exit;
	}

	ret = get_efi_variable(&guid, varname, &size, &ctx->private, &flags);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Cannot access EFI variable %a %g", argv[0], &guid);
		goto exit;
	}

	ctx->cur = 0;
	ctx->len = size;

exit:
	FreePool(varname);
	return ret;
}

/* Interface */
static EFI_STATUS read_from_private(reader_ctx_t *ctx, unsigned char **buf,
				    __attribute__((__unused__)) UINTN *len)
{
	*buf = (unsigned char *)ctx->private + ctx->cur;
	return EFI_SUCCESS;
}

static void free_private(reader_ctx_t *ctx)
{
	FreePool(ctx->private);
}

struct reader {
	const char *name;
	EFI_STATUS (*open)(reader_ctx_t *ctx, UINTN argc, char **argv);
	EFI_STATUS (*read)(reader_ctx_t *ctx, unsigned char **buf, UINTN *len);
	void (*close)(reader_ctx_t *ctx);
} READERS[] = {
	{ "ram",	ram_open,	ram_read,		ram_close },
	{ "acpi",	acpi_open,	read_from_private,	NULL },
	{ "part",	part_open,	part_read,		free_private },
	{ "efivar",	efivar_open,	read_from_private,	free_private }
};

#define MAX_ARGS		8
#define READER_DELIMITER	":"

EFI_STATUS reader_open(reader_ctx_t *ctx, char *args)
{
	UINTN argc;
	UINTN i;
	char *argv[MAX_ARGS], *token, *saveptr;
	struct reader *reader = NULL;

	if (!args || !ctx)
		return EFI_INVALID_PARAMETER;

	argv[0] = strtok_r((char *)args, READER_DELIMITER, &saveptr);
	if (!argv[0])
		return EFI_INVALID_PARAMETER;

	for (argc = 1; argc < ARRAY_SIZE(argv); argc++) {
		token = strtok_r(NULL, READER_DELIMITER, &saveptr);
		if (!token)
			break;
		argv[argc] = token;
	}

	if (token && strtok_r(NULL, READER_DELIMITER, &saveptr))
		return EFI_INVALID_PARAMETER;

	for (i = 0; i < ARRAY_SIZE(READERS); i++)
		if (!strcmp((CHAR8 *)argv[0], (CHAR8 *)READERS[i].name)) {
			reader = &READERS[i];
			break;
		}

	if (!reader)
		return EFI_UNSUPPORTED;

	ctx->reader = reader;
	return reader->open(ctx, argc - 1, argv + 1);
}

EFI_STATUS reader_read(reader_ctx_t *ctx, unsigned char **buf, UINTN *len)
{
	EFI_STATUS ret;

	if (!ctx || !len || !*len || !ctx->reader)
		return EFI_INVALID_PARAMETER;

	*len = min(*len, ctx->len - ctx->cur);
	if (*len == 0)
		return EFI_SUCCESS;

	ret = ctx->reader->read(ctx, buf, len);
	if (EFI_ERROR(ret))
		return ret;

	ctx->cur += *len;

	return EFI_SUCCESS;
}

void reader_close(reader_ctx_t *ctx)
{
	if (!ctx || !ctx->reader)
		return;

	if (ctx->reader->close)
		ctx->reader->close(ctx);
}
