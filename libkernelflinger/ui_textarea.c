/*
 * Copyright (c) 2014, Intel Corporation
 * All rights reserved.
 *
 * Authors:  Jeremy Compostella <jeremy.compostella@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
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

#include <efi.h>
#include <efilib.h>
#include <lib.h>

#include "ui.h"

static EFI_STATUS ui_textarea_allocate_blt(ui_textarea_t *textarea)
{
	UINTN blt_size;

	textarea->width = textarea->font->cwidth * textarea->row_nb;
	textarea->height = textarea->font->cheight * textarea->line_nb;

	blt_size = sizeof(*textarea->blt) * textarea->width * textarea->height;
	textarea->blt = AllocateZeroPool(blt_size);
	if (!textarea->blt)
		return EFI_OUT_OF_RESOURCES;

	return EFI_SUCCESS;
}

ui_textarea_t *ui_textarea_create(UINTN line_nb, UINTN row_nb, ui_font_t *font,
				  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *color)
{
	UINTN text_size;

	if (!font)
		font = ui_font_get_default();

	ui_textarea_t *textarea = AllocatePool(sizeof(ui_textarea_t));
	if (!textarea)
		return NULL;

	textarea->line_nb = line_nb;
	textarea->row_nb = row_nb;
	textarea->font = font;

	if (EFI_ERROR(ui_textarea_allocate_blt(textarea))) {
		FreePool(textarea);
		return NULL;
	}

	text_size = sizeof(*textarea->text) * line_nb;
	textarea->text = AllocateZeroPool(text_size);
	if (!textarea->text) {
		FreePool(textarea->blt);
		FreePool(textarea);
		return NULL;
	}

	textarea->current = -1;
	textarea->color = color;

	return textarea;
}

static void ui_textarea_copy_char(unsigned char *src_p, UINTN src_row_bytes,
				  unsigned char *dst_p, UINTN dst_row_bytes,
				  int width, int height,
				  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *color)
{
	int i, j;

	for (j = 0; j < height; ++j) {
		unsigned char* sx = src_p;
		unsigned char* px = dst_p;
		for (i = 0; i < width; ++i) {
			unsigned char a = *sx++;
			if (a == 255) {
				*px++ = color->Red;
				*px++ = color->Green;
				*px++ = color->Blue;
				px++;
			} else if (a > 0) {
				*px = (*px * (255-a) + color->Red * a) / 255;
				++px;
				*px = (*px * (255-a) + color->Green * a) / 255;
				++px;
				*px = (*px * (255-a) + color->Blue * a) / 255;
				++px;
				++px;
			} else {
				px += 4;
			}
		}
		src_p += src_row_bytes;
		dst_p += dst_row_bytes;
	}
}

static void ui_textarea_refresh_blt(ui_textarea_t *textarea)
{
	UINTN cur, i, j, x, y = 0;
	ui_font_t *font = textarea->font;
	UINTN pixel_size = sizeof(*textarea->blt);
	UINTN row_size = textarea->width * pixel_size;
	EFI_GRAPHICS_OUTPUT_BLT_PIXEL *color;

	ZeroMem(textarea->blt,
		textarea->width * textarea->height * sizeof(*textarea->blt));

	for (i = 1; i <= textarea->line_nb; i++) {
		cur = (textarea->current + i) % textarea->line_nb;

		color = textarea->color;
		if (textarea->text[cur].color)
			color = textarea->text[cur].color;

		unsigned char *s = (unsigned char *)textarea->text[cur].str;
		for (x = 0, j = 0; s && *s && j < textarea->row_nb; s++, x += font->cwidth, j++) {
			if (*s <= 0x20 || *s > 0x7E)
				continue;
			if (*s == '\n')
				break;

			unsigned char* src_p = font->texture + ((*s - 0x20) * font->cwidth)
				+ (textarea->text[cur].bold ? font->cheight * font->width : 0);
			unsigned char* dst_p = ((unsigned char *)textarea->blt)
				+ (y * row_size)
				+ (x * pixel_size);

			ui_textarea_copy_char(src_p, font->width, dst_p, row_size,
					      font->cwidth, font->cheight, color);
		}
		y += font->cheight;
	}
}

EFI_STATUS ui_textarea_display_text(const ui_textline_t *text, ui_font_t *font,
				    UINTN x, UINTN *y)
{
	ui_textarea_t textarea;
	EFI_STATUS ret;
	UINTN line_nb, len, row_nb = 0;

	for (line_nb = 0; text[line_nb].str; line_nb++) {
		if (!text[line_nb].str)
			continue;
		len = strlen((CHAR8 *)text[line_nb].str);
		row_nb = row_nb < len ? len : row_nb;
	}

	textarea.line_nb = line_nb;
	textarea.row_nb = row_nb;
	textarea.text = (ui_textline_t *)text;
	textarea.color = NULL;
	textarea.font = font;
	textarea.current = -1;

	ret = ui_textarea_allocate_blt(&textarea);
	if (EFI_ERROR(ret))
		return ret;

	ui_textarea_draw(&textarea, x, *y);

	*y += textarea.height;

	FreePool(textarea.blt);

	return EFI_SUCCESS;
}

void ui_textarea_free(ui_textarea_t *textarea)
{
	UINTN i;

	for (i = 0; i < textarea->line_nb; i++)
		FreePool(textarea->text[i].str);

	FreePool(textarea->blt);
	FreePool(textarea->text);
}

void ui_textarea_clear(ui_textarea_t *textarea)
{
	UINTN i;

	for (i = 0; i < textarea->line_nb; i++)
		if (textarea->text[i].str) {
			FreePool(textarea->text[i].str);
			textarea->text[i].str = NULL;
		}

	textarea->current = -1;
}

void ui_textarea_set_line(ui_textarea_t *textarea, UINTN line_nb, char *str,
			  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *color, BOOLEAN bold)
{
	textarea->text[line_nb].str = str;
	textarea->text[line_nb].color = color;
	textarea->text[line_nb].bold = bold;
}

void ui_textarea_newline(ui_textarea_t *textarea, char *str,
			 EFI_GRAPHICS_OUTPUT_BLT_PIXEL *color, BOOLEAN bold)
{
	textarea->current = (textarea->current + 1) % textarea->line_nb;

	if (textarea->text[textarea->current].str)
		FreePool(textarea->text[textarea->current].str);

	ui_textarea_set_line(textarea, textarea->current, str, color, bold);
}

EFI_STATUS ui_textarea_draw(ui_textarea_t *textarea, UINTN x, UINTN y)
{
	ui_textarea_refresh_blt(textarea);
	return ui_draw_blt(textarea->blt, x, y, textarea->width, textarea->height);
}
