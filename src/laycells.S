; SPDX-License-Identifier: GPL-3.0-or-later

; A figure's cells for one screen row, into a row under construction.
;
; extern "C" void lay_cells(uint8_t *screen, uint8_t *colour, uint8_t cells,
;                           uint8_t flags, uint16_t first, uint8_t step,
;                           uint8_t colour_byte)
;
; The inner loop of rrb::RowList::layer, which stays the reference: the host
; runs it. Cell i is glyph first + i*step, its screen bytes the number's low
; byte and its top five bits, its colour bytes flags and colour_byte (rrb.hpp,
; glyph()). A mirrored figure (flags & FLIP_HORIZONTAL) takes the same cells
; right to left, so there is no multiply for where it starts.
;
; Assembly because the same loop in C costs about 300 cycles a cell, most of
; a row, stubbed and measured on the machine.
;
; ABI (llvm-mos): screen in __rs1, colour in __rs2, cells in A, flags in X,
; first in __rc6/__rc7, step in __rc8, colour_byte in __rc9. A leaf using
; A, X, Y and __rc2-__rc11, all caller-saved. Z is untouched, so stays nought.

#include "banknumbers.hpp"

	.section BANK_SECTION(AGOS_DISPLAY_BANK),"ax",@progbits
	.globl	lay_cells
	.type	lay_cells,@function

#define SCREEN __rc2
#define COLOUR __rc4
#define NUMBER mos8(__rc6)
#define NUMBER_HI mos8(__rc7)
#define STEP mos8(__rc8)
#define TINT mos8(__rc9)
#define COUNT mos8(__rc10)
#define FLAGS mos8(__rc11)
; Added to Y after each cell's two bytes: nought onwards, -4 back.
#define BACK mos8(__rc11)

FLIP_HORIZONTAL = $40		; rrb.hpp; viciv.vhdl:4400

lay_cells:
	sta	COUNT
	tay
	beq	done			; a figure wholly clipped has no cells here
	stx	FLAGS
	lda	#0
	bit	FLAGS			; V is bit 6, the mirror
	bvc	forward
	dey				; the last cell's offset, (cells - 1) * 2
	tya
	asl
	tay
	lda	#$fc
	.byte	$2c			; bit abs: skips the ldy below
forward:
	ldy	#0
	; Flags are only needed as a byte from here, so X carries them and
	; their slot holds the step back.
	sta	BACK
loop:
	lda	NUMBER
	sta	(SCREEN),y
	txa
	sta	(COLOUR),y
	iny
	lda	NUMBER_HI
	and	#$1f			; the top three bits are a width trim
	sta	(SCREEN),y
	lda	TINT
	sta	(COLOUR),y
	iny
	tya
	clc
	adc	BACK
	tay
	clc
	lda	NUMBER
	adc	STEP
	sta	NUMBER
	bcc	1f
	inc	NUMBER_HI
1:
	dec	COUNT
	bne	loop
done:
	rts
	.size	lay_cells, .-lay_cells
