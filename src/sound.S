; Channel 3's ring, kept full from the raster interrupt: speech, and an
; effect added into it.
;
; extern "C" void sound_tick(void)   refill what the playback has left
; extern "C" void sound_fill(void)   fill ring page sound_page, from the
;                                    main loop while sound_live is nought
;
; Speech and effects play through one 4 KB ring in chip RAM, because audio
; DMA cannot see the Attic where they are (iomap.txt:1103), and through one
; channel, because the CD32 plays both on its channel 3 (runit2). The
; hardware loops the ring by itself; this refills, a page at a time, every
; page the play position has left behind. A page is the speech's next page,
; by DMA, or silence; then the effect's next page is added into it by the
; CPU, a byte at a time. The CD32 adds an effect into its speech buffer the
; same way (runit2 0x1ccce) but lets the sum wrap, and many effects peak at
; full scale: each overflow is a click. This clips instead, as ScummVM's
; mixer does (sound.cpp:503). DMAgic cannot add (its mix command is
; unimplemented, gs4510.vhdl:6037), and the effect is read straight from the
; Attic through a 32-bit pointer, so no near buffer is needed.
;
; The refill runs a ringful behind the playback, so a count of pages filled
; without something is a count of pages played without it: once a whole
; ring has gone down with nothing, the channel stops, and once a ring has
; gone without speech, the voice is over -- IF_SPEECH asks sound_quiet.
; While the main loop is still reading speech off the card, a page it has
; not reached yet is silence but not the end: the speech resumes where it
; was.
;
; From the interrupt rather than the main loop, which a zone load holds up
; for 119-292 ms against the ring's 186 ms at 22 kHz. A DMA job of its own,
; so an interrupt landing inside a main-loop move cannot rewrite the list
; that move is about to run; move.hpp masks interrupts over its trigger
; writes, so this cannot land between them either.
;
; ABI: A and Z, absolute operands and two zero-page pointers declared __zp
; in C++, so the compiler's allocation leaves room for them. No __rc. Z
; comes back nought, where the add's loop ends it, as compiled code needs.

#include "chipmap.hpp"

#define CH3_FLAGS 0xd750        // DMA:CH3EN and kin (iomap.txt)
#define CH3_CURADDR_PAGE 0xd75b // current address, bits 8-15
#define DMA_ADDRMSB 0xd701
#define DMA_ADDRBANK 0xd702
#define DMA_F018B 0xd703
#define DMA_ETRIG 0xd705        // list address low byte, and go

#define RING_PAGE ((SIMON_SPEECH >> 8) & 0xff)
#define RING_BANK (SIMON_SPEECH >> 16)
#define RING_PAGES 16           // chipmap::SPEECH_BYTES / 256
#define BANKS_A_MEGABYTE 16
#define DMA_COPY 0x00
#define DMA_FILL 0x03

	.section .data.sound,"aw",@progbits

; The speech's job. Its source fields are the speech's position: page-
; aligned, so a page never crosses a megabyte and each field steps alone.
	.globl	sound_job, sound_src_megabyte, sound_src_bank, sound_src_page
sound_job:
	.byte	0x0b			; F018B list
	.byte	0x80			; source megabyte:
sound_src_megabyte:
	.byte	0
	.byte	0x81, 0			; destination megabyte: chip
	.byte	0x85, 1			; destination step
	.byte	0			; end of options
sound_job_command:
	.byte	DMA_COPY
	.2byte	256
	.byte	0			; source low byte, and the fill value
sound_src_page:
	.byte	0
sound_src_bank:
	.byte	0
	.byte	0			; destination low byte
sound_job_dest_page:
	.byte	RING_PAGE
	.byte	RING_BANK
	.byte	0			; command high byte
	.2byte	0			; modulo


	.section .bss.sound,"aw",@nobits

	.globl	sound_live, sound_page, sound_left, sound_hushed, sound_loading
	.globl	sound_effect_left, sound_quiet, sound_playing
sound_live:				; nought while the main loop sets up
	.zero	1
sound_loading:				; set while more speech is coming
	.zero	1
sound_page:				; the ring page to fill next
	.zero	1
sound_left:				; pages of speech not yet in the ring
	.zero	2
sound_effect_left:			; pages of effect not yet added
	.zero	2
sound_hushed:				; pages filled with nothing, in a row
	.zero	1
sound_quiet:				; pages filled without speech, in a row
	.zero	1
sound_playing:				; the page the hardware is in
	.zero	1

	.section .text.sound,"ax",@progbits
	.globl	sound_tick
	.type	sound_tick,@function
sound_tick:
	lda	mos16(sound_live)
	beq	.Ldone
	; One byte of a moving counter cannot tear. At the top it reads one past
	; the ring, which the mask makes page nought, where it is about to be.
	lda	CH3_CURADDR_PAGE
	sec
	sbc	#RING_PAGE
	and	#RING_PAGES - 1
	sta	mos16(sound_playing)
.Lnext:
	lda	mos16(sound_page)
	cmp	mos16(sound_playing)
	beq	.Ldone
	jsr	sound_fill
	lda	mos16(sound_hushed)
	cmp	#RING_PAGES
	bcc	.Lnext
	lda	#0			; a ringful of nothing has played
	sta	CH3_FLAGS
	sta	mos16(sound_live)
.Ldone:
	rts

; Fill ring page sound_page and step it on. sound_hushed counts on, and
; anything laid in the page sets it back to nought. sound_ring's other bytes
; are set once, by sound::start.
	.globl	sound_fill
	.type	sound_fill,@function
sound_fill:
	lda	mos16(sound_page)
	clc
	adc	#RING_PAGE
	sta	mos16(sound_job_dest_page)
	sta	sound_ring + 1
	inc	mos16(sound_hushed)

	lda	mos16(sound_left)
	ora	mos16(sound_left + 1)
	beq	.Lno_speech
	lda	#DMA_COPY
	jsr	.Lrun
	inc	mos16(sound_src_page)
	bne	.Lcounted
	inc	mos16(sound_src_bank)
	lda	mos16(sound_src_bank)
	cmp	#BANKS_A_MEGABYTE
	bne	.Lcounted
	lda	#0
	sta	mos16(sound_src_bank)
	inc	mos16(sound_src_megabyte)
.Lcounted:
	lda	mos16(sound_left)
	bne	.Lless
	dec	mos16(sound_left + 1)
.Lless:
	dec	mos16(sound_left)
	lda	#0
	sta	mos16(sound_quiet)
	sta	mos16(sound_hushed)
	bra	.Leffect

.Lno_speech:
	lda	#DMA_FILL
	jsr	.Lrun
	lda	mos16(sound_loading)
	beq	.Lsilent
	lda	#0			; starved, not finished
	sta	mos16(sound_hushed)
	bra	.Leffect
.Lsilent:
	lda	mos16(sound_quiet)
	cmp	#RING_PAGES
	bcs	.Leffect
	inc	mos16(sound_quiet)

.Leffect:
	lda	mos16(sound_effect_left)
	ora	mos16(sound_effect_left + 1)
	beq	.Lcount
	; The effect's page added into the ring's, clipped. On a signed overflow
	; the carry says which way: clear, two positives, so 127; set, two
	; negatives, so -128. Z runs a page and ends at nought.
	ldz	#0
.Ladd:
	lda	[sound_effect],z
	clc
	adc	[sound_ring],z
	bvc	.Lsummed
	lda	#0x7f
	adc	#0			; 127, or -128 with the carry
.Lsummed:
	sta	[sound_ring],z
	inz
	bne	.Ladd
	inc	sound_effect + 1
	bne	.Lstepped
	inc	sound_effect + 2
	bne	.Lstepped
	inc	sound_effect + 3
.Lstepped:
	lda	mos16(sound_effect_left)
	bne	.Lfewer
	dec	mos16(sound_effect_left + 1)
.Lfewer:
	dec	mos16(sound_effect_left)
	lda	#0
	sta	mos16(sound_hushed)

.Lcount:
	lda	mos16(sound_page)
	clc
	adc	#1
	and	#RING_PAGES - 1
	sta	mos16(sound_page)
	rts

; Run the speech's job as command A. DMA stops the CPU until it is done.
.Lrun:
	sta	mos16(sound_job_command)
	lda	#1
	sta	DMA_F018B
	lda	#0
	sta	DMA_ADDRBANK
	lda	#mos16hi(sound_job)
	sta	DMA_ADDRMSB
	lda	#mos16lo(sound_job)
	sta	DMA_ETRIG
	rts
