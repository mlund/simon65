;;; Peppito MOD Playback Driver v0.16A, by Daniel England of Ecclestial
;;; Solutions, copyright 2021. Ported from ca65 to llvm-mos by way of
;;; MirageBD's AtticMod fork: the module is read from Attic RAM through 32-bit
;;; indirect addressing, while its sample block is copied to `sampledata` in
;;; chip RAM, because audio DMA's base address is only 24 bits wide
;;; (iomap.txt:1103-1105).
;;;
;;; Unlike both, the driver's zero page is a base page of its own, reached with
;;; TAB, so its fixed $e0/$f0 pointers cannot collide with the compiler's.

;;; ---------------------------------------------------------------------------
;;; Hardware, named from mega65-core iomap.txt
;;; ---------------------------------------------------------------------------

#define VIC_BORDERCOL 0xd020

;;; Audio DMA.  One 16-byte block per channel, indexed by X throughout.
#define AUDIO_DMA_ENABLE 0xd711
#define AUDEN 0x80  // DMA:AUDEN
#define PCM_BYPASS 0x10  // drives the amp straight, around the mixer
#define AUDIO_DMA_MISC 0xd712  // only CHANSWAP, bit 1, is implemented
#define MATH_DIVBUSY 0xd70f  // bit 7 set while the divider is working
#define AUDIO_CH0 0xd720
#define AUDIO_CH_STRIDE 0x10  // one block per channel
#define CHXFLAGS 0  // SBITS, STP, SINE, USGN, LOOP, EN
#define CHXBADDR 1  // base address, 3 bytes
#define CHXFREQ 4  // frequency, 3 bytes
#define CHXTADDR 7  // top address, 2 bytes
#define CHXVOLUME 9
#define CHXCURADDR 10  // current address, 3 bytes
#define CH0RVOL 0xd71c  // then CH1RVOL, CH2LVOL, CH3LVOL
;;; Music keeps channels 0-2. The CD32's player sends its fourth voice to a
;;; dummy (runit2 0x20266, DMACON masked to 7 at 0x1fcf8), and speech and
;;; effects take channel 3 (sound.hpp): its effect column still steers the song,
;;; but nothing of it reaches the hardware.
#define MUSIC_CHANNELS 3

;;; The MATH unit, used for the period and rate arithmetic.
#define MATH_MULTINA 0xd770  // also the divider's numerator
#define MATH_MULTINB 0xd774  // also the divider's denominator
#define MATH_MULTOUT 0xd778
#define MATH_DIVOUT 0xd768

;;; ---------------------------------------------------------------------------
;;; Module layout offsets (fixed by format, not configurable).
;;; ---------------------------------------------------------------------------

#define MOD_SAMPLE_HDRS 0x0014  // 31 headers of 30 bytes, after the title
#define MOD_ORDERS 0x0438  // song length, restart, 128 order bytes
#define SMPHDR_LENGTH 0x16  // within a sample header: length, words
#define SMPHDR_FINETUNE 0x18
#define SMPHDR_LOOPSTART 0x1a
#define SMPHDR_LOOPLEN 0x1c

;;; Amiga clock over the MEGA65's sample rate, as fixed point.
#define VAL_FAC_AMIGARATEL 0xab6f
#define VAL_FAC_AMIGARATEH 0x000d
#define VAL_FAC_M65RATERTL 0xa395
#define VAL_FAC_M65RATERTH 0x0001

;;; ---------------------------------------------------------------------------
;;; The driver's own base page.  TAB points at it for the duration of a call,
;;; so these are offsets, not addresses, and every access is two bytes rather
;;; than three.  Nothing relaxes absolute into base page at link time.
;;; ---------------------------------------------------------------------------

#define ptrPepTmpA 0x00  // 2
#define ptrPepChan 0x02  // 2
#define ptrPepModF 0x10  // 4, into the module in Attic
#define ptrPepMSeq 0x14  // 4
#define ptrPepPtrn 0x18  // 4
#define ptrPepSmpD 0x1c  // 4, into sampledata in chip RAM

;;; The module's 32-bit address, and everything the row and tick paths touch.
;;; These were absolute: three bytes and four cycles an access, against two and
;;; three here. The scratch pair alone is referenced 186 times.
;;;
;;; What may NOT move here is anything an absolute-only instruction touches --
;;; `asw` is `cb nn nn` with no base-page form, so `asw 0x24` would address
;;; $0024 rather than pep_page+$24. That is why __PEP_ASL_MEM16 went back to
;;; bytes: eight bytes a site, seven sites, to buy 186.
#define adrPepMODL 0x20  // 4, low word then high, as C++ writes them
#define valPepTmp0 0x24  // 2 each, the row and tick scratch
#define valPepTmp1 0x26
#define valPepTmp2 0x28
#define valPepTmp3 0x2a
#define valPepTmp4 0x2c
#define cntPepTick 0x30
#define cntPepPRow 0x31
#define cntPepSeqP 0x32
#define valPepSped 0x33
#define valPepPBrk 0x34
#define valPepNRow 0x35
#define valPepSLen 0x36
#define valPepSRst 0x37
#define valPepChan 0x38
#define valPepMaxI 0x39
#define valPepMaxP 0x3a
#define cntPepPtnL 0x3b
#define valPepPLCh 0x3c
#define valPepMRg0 0x44  // 8, the MATH unit saved across a tick

;;; `sampledata` comes from chipmap.hpp; audio DMA cannot reach Attic RAM so
;;; the sample block is copied here. This .S file is preprocessed.

#include "chipmap.hpp"
	.equ sampledata, SIMON_SAMPLES

#ifndef PEP_SAVE_MATH
#define PEP_SAVE_MATH 1  // give the MATH unit back as the player found it
#endif

#ifndef PEP_SHOW_RASTER
#define PEP_SHOW_RASTER 0  // 1 paints the border across a tick
#endif

;;; ---------------------------------------------------------------------------
;;; Record layouts.  INSMHDR is the module's own sample header; the rest are
;;; this driver's, and the assert is what keeps a channel record indexable by
;;; a single byte.
;;; ---------------------------------------------------------------------------

;;; PEP_INSMHDR, 30 bytes
#define PEP_INSMHDR_strName 0
#define PEP_INSMHDR_bewSampLen 22
#define PEP_INSMHDR_sbyFineTune 24
#define PEP_INSMHDR_sbyVolume 25
#define PEP_INSMHDR_bewLoopStrt 26
#define PEP_INSMHDR_bewLoopLen 28
#define PEP_INSMHDR_size 30

;;; PEP_INSDATA, 16 bytes
#define PEP_INSDATA_ptrHdr 0
#define PEP_INSDATA_valVol 4
#define PEP_INSDATA_valFTune 5
#define PEP_INSDATA_valSLen 6
#define PEP_INSDATA_valLStrt 8
#define PEP_INSDATA_valLLen 10
#define PEP_INSDATA_ptrSmpD 12
#define PEP_INSDATA_size 16

;;; PEP_NOTDATA, 5 bytes
#define PEP_NOTDATA_valKey 0
#define PEP_NOTDATA_valIns 2
#define PEP_NOTDATA_valEff 3
#define PEP_NOTDATA_valPrm 4
#define PEP_NOTDATA_size 5

;;; PEP_CHNDATA, 22 bytes
#define PEP_CHNDATA_recNote 0
#define PEP_CHNDATA_valVol 5
#define PEP_CHNDATA_valFTune 6
#define PEP_CHNDATA_valAssIns 7
#define PEP_CHNDATA_valSelIns 8
#define PEP_CHNDATA_valSmpOffs 9
#define PEP_CHNDATA_valPeriod 11
#define PEP_CHNDATA_valPrtPer 13
#define PEP_CHNDATA_valPrtSpd 15
#define PEP_CHNDATA_valVtPhs 16
#define PEP_CHNDATA_valVibAdd 17
#define PEP_CHNDATA_valVibSpd 18
#define PEP_CHNDATA_valVibDep 19
#define PEP_CHNDATA_valFxCnt 20
#define PEP_CHNDATA_valPtnLRow 21
#define PEP_CHNDATA_size 22

;;; ---------------------------------------------------------------------------
;;; Pointer arithmetic.  The module lives in Attic RAM, so every pointer into
;;; it is 32 bits and carries through four bytes.
;;; ---------------------------------------------------------------------------

	;; ptr = adrPepMOD + offs, done in Q -- the 45GS02's A:X:Y:Z acting as one
	;; 32-bit register (appendix-45gs02-registers.tex:424). Thirteen
	;; instructions become four, 29 bytes become 15, eight times over.
	;;
	;; **This clobbers X, Y and Z as well as A.** All eight call sites reload
	;; each of them before next use; a ninth has to be checked the same way,
	;; and nothing in the toolchain will say so if it is not. There is no adcq
	;; immediate, so the offset is assembled as a constant beside the code.
	.macro __PEP_SET_MODF_OFFS ptr, offs
	.pushsection .rodata.pep,"a",@progbits
99:	.4byte \offs
	.popsection
	clc
	ldq adrPepMODL
	adcq mos16(99b)
	stq \ptr
	.endm

	.macro __PEP_ADD_PTR32_IMM16 ptr, offs
	clc
	lda #<\offs
	adc \ptr
	sta \ptr
	lda #>\offs
	adc \ptr+1
	sta \ptr+1
	lda #0x00
	adc \ptr+2
	sta \ptr+2
	lda #0x00
	adc \ptr+3
	sta \ptr+3
	.endm

	.macro __PEP_ADD_PTR32_MEM16 ptr, mem
	clc
	lda \mem
	adc \ptr
	sta \ptr
	lda \mem+1
	adc \ptr+1
	sta \ptr+1
	lda #0x00
	adc \ptr+2
	sta \ptr+2
	lda #0x00
	adc \ptr+3
	sta \ptr+3
	.endm

	.macro __PEP_ADD_PTR16_MEM16 ptr, mem
	clc
	lda \mem
	adc \ptr
	sta \ptr
	lda \mem+1
	adc \ptr+1
	sta \ptr+1
	.endm

	.macro __PEP_ADD_PTR16_IMM16 ptr, offs
	clc
	lda #<\offs
	adc \ptr
	sta \ptr
	lda #>\offs
	adc \ptr+1
	sta \ptr+1
	.endm

	;; Bytes rather than `asw`, because the operands now live in the base page
	;; and `asw` is absolute-only. Eleven bytes here against three, seven
	;; times -- paid for many times over by the accesses it unblocks.
	.macro __PEP_ASL_MEM16 mem
	clc
	lda \mem
	asl
	sta \mem
	lda \mem+1
	rol
	sta \mem+1
	.endm

	.macro __PEP_LSR_MEM16 mem
	clc
	lda \mem+1
	lsr
	sta \mem+1
	lda \mem
	ror
	sta \mem
	.endm

	;; Store a 32-bit pointer through (ind),Y, leaving Y past it.
	.macro __PEP_MOV_PTR32_IND32 ptr, ind
	lda \ptr
	sta (\ind),y
	iny
	lda \ptr+1
	sta (\ind),y
	iny
	lda \ptr+2
	sta (\ind),y
	iny
	lda \ptr+3
	sta (\ind),y
	iny
	.endm

;;; ---------------------------------------------------------------------------
;;; The MATH unit.  Loading both inputs starts the operation; the result is
;;; read from MULTOUT or DIVOUT.  The divider is not ready on the next cycle,
;;; hence the eight wasted accesses -- the author's own way of waiting.
;;; ---------------------------------------------------------------------------

	;; Upstream waited for the divider with eight dummy $D020 accesses, a
	;; guess. The core publishes a ready flag, and xemu answers instantly --
	;; so a wait that is too short is invisible there and only wrong on
	;; hardware, where a stale result is a wrong playback frequency.
	.macro __PEP_MATH_SETTLE
99:	bit MATH_DIVBUSY
	bmi 99b
	.endm

	.macro __PEP_MUL_MEM8_MEM8 mem0, mem1
	lda \mem0
	sta MATH_MULTINA
	lda #0x00
	sta MATH_MULTINA+1
	sta MATH_MULTINA+2
	sta MATH_MULTINA+3

	lda \mem1
	sta MATH_MULTINB
	lda #0x00
	sta MATH_MULTINB+1
	sta MATH_MULTINB+2
	sta MATH_MULTINB+3
	.endm

	.macro __PEP_MUL_MEM16_MEM16 mem0, mem1
	lda \mem0
	sta MATH_MULTINA
	lda \mem0+1
	sta MATH_MULTINA+1
	lda #0x00
	sta MATH_MULTINA+2
	sta MATH_MULTINA+3

	lda \mem1
	sta MATH_MULTINB
	lda \mem1+1
	sta MATH_MULTINB+1
	lda #0x00
	sta MATH_MULTINB+2
	sta MATH_MULTINB+3
	.endm

	.macro __PEP_DIV_MEM16_IMM16 mem, imm
	lda \mem
	sta MATH_MULTINA
	lda \mem+1
	sta MATH_MULTINA+1
	lda #0x00
	sta MATH_MULTINA+2
	sta MATH_MULTINA+3

	lda #<\imm
	sta MATH_MULTINB
	lda #>\imm
	sta MATH_MULTINB+1
	lda #0x00
	sta MATH_MULTINB+2
	sta MATH_MULTINB+3

	__PEP_MATH_SETTLE
	.endm

	.macro __PEP_DIV_MEM32_IMM16 mem, imm
	lda \mem
	sta MATH_MULTINA
	lda \mem+1
	sta MATH_MULTINA+1
	lda \mem+2
	sta MATH_MULTINA+2
	lda \mem+3
	sta MATH_MULTINA+3

	lda #<\imm
	sta MATH_MULTINB
	lda #>\imm
	sta MATH_MULTINB+1
	lda #0x00
	sta MATH_MULTINB+2
	sta MATH_MULTINB+3

	__PEP_MATH_SETTLE
	.endm

;;; ---------------------------------------------------------------------------
;;; What C++ calls.  Each moves the base page to the driver's own window for
;;; the duration, and puts B and Z back before returning -- every imaginary
;;; register is base-page addressed, so compiled code cannot run while B is
;;; moved, and a stray Z offsets every pointer dereference program-wide.
;;; Nothing in the toolchain checks either.
;;;
;;; AtticMod's TBL_PEP_JUMP is dropped. The three entry routines carry the
;;; base-page move themselves rather than being wrapped a call away: nothing
;;; inside the driver calls them, so each has exactly one caller and a stub
;;; bought only a jsr and an rts.
;;; ---------------------------------------------------------------------------

	.section .text.pep_entry,"ax",@progbits

	;; Interrupts are held off across the moved base page and the caller's
	;; flag restored, so a caller that had them off keeps them off.
	.macro PEP_ENTER
	php
	sei
	lda #mos16hi(pep_page)
	tab
	.endm

	;; Z is put back first so that A can be had from it: `tza` is a byte where
	;; `lda #0` is two, and here -- unlike everywhere else in this driver --
	;; Z is provably nought, because the line above just made it so.
	.macro PEP_LEAVE
	ldz #0x00
	tza
	tab
	plp
	.endm

;;; void pep_init(void) -- read the module at adrPepMOD* and set the channels up.
	.global pep_init

;;; void pep_play(void) -- one tick; call at 50 Hz.
	.global pep_play

;;; void pep_stop(void) -- silence the channels and stop audio DMA.
	.global pep_stop

;;; uint8_t pep_bp_works(void) -- $5A if TAB really moved the base page.
;;; Neither the toolchain nor the emulator says whether it took, and everything
;;; above rests on it.
	.global pep_bp_works
pep_bp_works:
	PEP_ENTER
	lda #0x5a
	sta ptrPepTmpA
	PEP_LEAVE
	lda pep_page+ptrPepTmpA
	ldx #0x00
	rts

;;; The driver's body. PEP_ATTIC_BANK moves it to bank 13, the first Attic
;;; bank ($8000000), to find out what ten-times-slower memory costs a tick.
;;; A section name cannot be pasted together by the preprocessor, so the bank
;;; number is spelt out here and checked against the build's PEP_BANK.
#ifdef PEP_ATTIC_BANK
#if PEP_BANK != 13
#error "PEP_BANK and this section name disagree"
#endif
	.section .bank_13.text.pep,"ax",@progbits
#else
	.section .text.pep,"ax",@progbits
#endif

pep_init:
;;; Read module at adrPepMOD and set channels up.
peppitoInit:
	PEP_ENTER

	lda #0x00
	sta valPepPBrk
	sta valPepNRow

	sta cntPepSeqP

	lda #0x01
	sta cntPepTick

	lda #0x06
	sta valPepSped

	lda #0x00
	sta valPepMaxP

	lda #0xff
	sta cntPepPtnL
	sta valPepPLCh

	__PEP_SET_MODF_OFFS ptrPepModF, MOD_ORDERS
	ldz #0x00
	lda [ptrPepModF],z
	sta valPepTmp0
	inz
	lda [ptrPepModF],z
	sta valPepTmp0+1
	inz
	lda [ptrPepModF],z
	sta valPepTmp0+2
	inz
	lda [ptrPepModF],z
	sta valPepTmp0+3

	lda #0x10
	sta valPepMaxI

	ldz #0x00
.LpeppitoInit_loop1:
	;; X walks the signature we read, Y the table, which is the opposite way
	;; round from upstream. It has to be: the signature now sits in the base
	;; page, and `cmp` has a zp,x form but no zp,y -- indexed by Y it would
	;; quietly read absolute $0024 instead.
	ldx #0x00

	tza
	asl
	asl
	tay

.LpeppitoInit_loop2:
	lda valPepSig0,y
	cmp valPepTmp0,x

	bne .LpeppitoInit_next1

	iny
	inx
	cpx #0x04
	bne .LpeppitoInit_loop2

	lda #0x20
	sta valPepMaxI
	jmp .LpeppitoInit_cont1

.LpeppitoInit_next1:
	inz
	cpz #0x04
	bne .LpeppitoInit_loop1

.LpeppitoInit_cont1:
	lda valPepMaxI
	cmp #0x10
	bne .LpeppitoInit_ins320

	__PEP_SET_MODF_OFFS ptrPepModF, 0x01d6

	jmp .LpeppitoInit_cont2

.LpeppitoInit_ins320:
	__PEP_SET_MODF_OFFS ptrPepModF, 0x03b6

.LpeppitoInit_cont2:
	ldz #0x00
	lda [ptrPepModF],z

	and #0x7f
	sta valPepSLen

	inz

	lda [ptrPepModF],z

	and #0x7f
	sta valPepSRst

	lda valPepSLen
	cmp valPepSRst
	bcs .LpeppitoInit_cont0

	lda #0x00
	sta valPepSRst

.LpeppitoInit_cont0:
	lda valPepMaxI
	cmp #0x10
	bne .LpeppitoInit_ins321

	__PEP_SET_MODF_OFFS ptrPepMSeq, 0x01d8
	jmp .LpeppitoInit_cont3

.LpeppitoInit_ins321:
	__PEP_SET_MODF_OFFS ptrPepMSeq, 0x03b8

.LpeppitoInit_cont3:
	ldx #0x00
	ldz #0x00
.LpeppitoInit_loop0:
	lda [ptrPepMSeq],z
	sta idxPepPtn0,x

	cmp valPepMaxP
	bcc .LpeppitoInit_next0

	sta valPepMaxP

.LpeppitoInit_next0:
	inx
	inz
	cpz #0x80
	bne .LpeppitoInit_loop0

	inc valPepMaxP

	jsr peppitoReadSeq

	jsr peppitoReadIns

	jsr peppitoClearChans

	PEP_LEAVE
	rts

pep_play:
;;; Advance one tick of playback (call at 50 Hz).
peppitoPlay:
	PEP_ENTER
#if PEP_SHOW_RASTER
	lda VIC_BORDERCOL
	sta valPepRstr

	lda #0x01
	sta VIC_BORDERCOL
#endif

	jsr peppitoSaveState

	dec cntPepTick
	bne .LpeppitoPlay_procTick

	lda valPepSped
	sta cntPepTick

	jsr peppitoPerformRow
	jmp .LpeppitoPlay_exit

.LpeppitoPlay_procTick:
#if PEP_SHOW_RASTER
	lda #0x04
	sta VIC_BORDERCOL
#endif

	jsr pepptioPerformTick

.LpeppitoPlay_exit:
#if PEP_SAVE_MATH
	jsr peppitoRestoreState
#endif

#if PEP_SHOW_RASTER
	lda valPepRstr
	sta VIC_BORDERCOL
#endif

	PEP_LEAVE
	rts

pep_stop:
;;; Silence music channels (speech on channel 3 continues).
peppitoStop:
	PEP_ENTER
	lda #0x00
	sta AUDIO_CH0+CHXFLAGS
	sta AUDIO_CH0+1 * AUDIO_CH_STRIDE+CHXFLAGS
	sta AUDIO_CH0+2 * AUDIO_CH_STRIDE+CHXFLAGS

	lda #0x01
	sta cntPepTick

	PEP_LEAVE
	rts

peppitoClearChans:
;;; Zero all channel records and disable audio DMA.
	ldx #0x00
.LpeppitoClearChans_loop0:
	phx
	txa
	asl
	tax

	lda idxPepChn0,x
	sta ptrPepTmpA
	lda idxPepChn0+1,x
	sta ptrPepTmpA+1

	ldy #0x00
	lda #0x00
.LpeppitoClearChans_loop1:
	sta (ptrPepTmpA),y
	iny

	cpy #PEP_CHNDATA_size
	bne .LpeppitoClearChans_loop1

	plx
	inx
	cpx #0x04
	bne .LpeppitoClearChans_loop0

	lda #0x00
	sta AUDIO_CH0+CHXFLAGS
	sta AUDIO_CH0+1 * AUDIO_CH_STRIDE+CHXFLAGS
	sta AUDIO_CH0+2 * AUDIO_CH_STRIDE+CHXFLAGS

	;; Clears CHANSWAP. iomap names bit 0 SATURATE, but the core writes only
	;; bit 1 (gs4510.vhdl:3427) and the mixer clips unconditionally in a 19-bit
	;; accumulator anyway (gs4510.vhdl:4215-4227), so there is nothing else here.
	lda #0x00
	sta AUDIO_DMA_MISC

	rts

peppitoSaveState:
;;; Save MATH unit registers.
	ldx #0x00

.LpeppitoSaveState_loop0:
	lda MATH_MULTINA,x
	sta valPepMRg0,x

	inx
	cpx #0x08
	bne .LpeppitoSaveState_loop0

	rts

peppitoRestoreState:
;;; Restore MATH unit registers and wait for ready.
	ldx #0x00

.LpeppitoRestoreState_loop0:
	lda valPepMRg0,x
	sta MATH_MULTINA,x

	inx
	cpx #0x08
	bne .LpeppitoRestoreState_loop0

	__PEP_MATH_SETTLE

	rts

peppitoReadIns:
;;; Read instrument headers from module, convert to playback records.
	__PEP_SET_MODF_OFFS ptrPepModF, MOD_SAMPLE_HDRS

	ldx #0x01
.LpeppitoReadIns_loop:

	txa
	asl
	tay
	lda idxPepIns0,y
	sta ptrPepTmpA+0
	iny
	lda idxPepIns0,y
	sta ptrPepTmpA+1

	ldy #PEP_INSDATA_ptrHdr
	__PEP_MOV_PTR32_IND32 ptrPepModF, ptrPepTmpA

	ldz #SMPHDR_LENGTH

	lda [ptrPepModF],z
	sta valPepTmp0+1
	inz

	lda [ptrPepModF],z
	sta valPepTmp0

	__PEP_ASL_MEM16 valPepTmp0

	ldy #PEP_INSDATA_valSLen
	lda valPepTmp0
	sta valPepTmp4
	sta (ptrPepTmpA),y
	iny
	lda valPepTmp0+1
	sta valPepTmp4+1
	sta (ptrPepTmpA),y

	ldy #PEP_INSDATA_ptrSmpD
	__PEP_MOV_PTR32_IND32 ptrPepSmpD, ptrPepTmpA

	__PEP_ADD_PTR32_MEM16 ptrPepSmpD, valPepTmp0

	ldz #SMPHDR_LOOPSTART

	lda [ptrPepModF],z
	sta valPepTmp0+1
	sta valPepTmp2+1
	inz

	lda [ptrPepModF],z
	sta valPepTmp0
	sta valPepTmp2
	inz

	__PEP_ASL_MEM16 valPepTmp0

	lda [ptrPepModF],z
	sta valPepTmp1+1
	inz

	lda [ptrPepModF],z
	sta valPepTmp1
	inz

	__PEP_ASL_MEM16 valPepTmp1

	clc
	lda valPepTmp0
	adc valPepTmp1
	sta valPepTmp3
	lda valPepTmp0+1
	adc valPepTmp1+1
	sta valPepTmp3+1

	lda valPepTmp4+1
	cmp valPepTmp3+1
	bcc .LpeppitoReadIns_bytes0
	bne .LpeppitoReadIns_skip0
	lda valPepTmp4
	cmp valPepTmp3
	bcc .LpeppitoReadIns_bytes0

	jmp .LpeppitoReadIns_skip0

.LpeppitoReadIns_bytes0:
	clc
	lda valPepTmp2
	adc valPepTmp1
	sta valPepTmp3
	lda valPepTmp2+1
	adc valPepTmp1+1
	sta valPepTmp3+1

	lda valPepTmp4+1
	cmp valPepTmp3+1
	bcc .LpeppitoReadIns_less0
	bne .LpeppitoReadIns_bytes1
	lda valPepTmp4
	cmp valPepTmp3
	bcc .LpeppitoReadIns_less0

.LpeppitoReadIns_bytes1:
;	loop len:= sample len - loop start
	sec
	lda valPepTmp4
	sbc valPepTmp0
	sta valPepTmp1
	lda valPepTmp4+1
	sbc valPepTmp0+1
	sta valPepTmp1+1

	jmp .LpeppitoReadIns_skip0

.LpeppitoReadIns_less0:
	lda valPepTmp2
	sta valPepTmp0
	lda valPepTmp2+1
	sta valPepTmp0+1

.LpeppitoReadIns_skip0:
	lda valPepTmp1+1
	bne .LpeppitoReadIns_cont1
	lda valPepTmp1
	cmp #04
	bcs .LpeppitoReadIns_cont1

	sec
	lda valPepTmp4
	sbc valPepTmp1
	sta valPepTmp0
	lda valPepTmp4+1
	sbc valPepTmp1+1
	sta valPepTmp0+1

.LpeppitoReadIns_cont1:
	ldy #PEP_INSDATA_valLStrt
	lda valPepTmp0
	sta (ptrPepTmpA),y
	iny
	lda valPepTmp0+1
	sta (ptrPepTmpA),y
	iny

	lda valPepTmp1
	sta (ptrPepTmpA),y
	iny
	lda valPepTmp1+1
	sta (ptrPepTmpA),y

	ldz #SMPHDR_FINETUNE
	lda [ptrPepModF],z
	and #0x0f
	sta valPepTmp0
	inz
	and #0x08
	sta valPepTmp0+1

	sec
	lda valPepTmp0
	and #0x07
	sbc valPepTmp0+1
	sta valPepTmp0

	clc
	adc #0x08

	ldy #PEP_INSDATA_valFTune
	sta (ptrPepTmpA),y

	lda [ptrPepModF],z
	and #0x7f

	cmp #0x40
	bcc .LpeppitoReadIns_skip1

	lda #0x40

.LpeppitoReadIns_skip1:
	ldy #PEP_INSDATA_valVol
	sta (ptrPepTmpA),y

	__PEP_ADD_PTR32_IMM16 ptrPepModF, 0x001e

	inx
	cpx valPepMaxI
	bne .LpeppitoReadIns_loop

	rts

peppitoReadSeq:
;;; Store 32-bit pointers to all patterns; set sampledata base address.
	lda #<adrPepPtn0+0
	sta ptrPepTmpA
	lda #>adrPepPtn0
	sta ptrPepTmpA+1

	lda valPepMaxI
	cmp #0x10
	bne .LpeppitoReadSeq_ins320

	__PEP_SET_MODF_OFFS ptrPepModF, 0x0258
	jmp .LpeppitoReadSeq_cont0

.LpeppitoReadSeq_ins320:
	__PEP_SET_MODF_OFFS ptrPepModF, 0x043c

.LpeppitoReadSeq_cont0:
	ldz #0x00
.LpeppitoReadSeq_loop0:
	ldy #0x00
	lda ptrPepModF
	sta (ptrPepTmpA),y
	iny
	lda ptrPepModF+1
	sta (ptrPepTmpA),y
	iny
	lda ptrPepModF+2
	sta (ptrPepTmpA),y
	iny
	lda ptrPepModF+3
	sta (ptrPepTmpA),y
;		INY

	__PEP_ADD_PTR16_IMM16 ptrPepTmpA, 0x04
	__PEP_ADD_PTR32_IMM16 ptrPepModF, 0x0400

	inz
	cpz valPepMaxP
	bne .LpeppitoReadSeq_loop0

	;; Upstream took the sample base from where the pattern walk ended, which
	;; is inside the module -- and so in Attic RAM, where audio DMA cannot
	;; reach.  The samples are copied down to `sampledata` in chip RAM before
	;; the driver starts, so point at that instead.  Its top byte is zero by
	;; construction: chip RAM ends at $60000.
	lda #mos16lo(sampledata)
	sta ptrPepSmpD+0
	lda #mos16hi(sampledata)
	sta ptrPepSmpD+1
	lda #mos24bank(sampledata)
	sta ptrPepSmpD+2
	lda #0x00
	sta ptrPepSmpD+3

	rts

peppitoChanPorta:
	ldy #PEP_CHNDATA_valPeriod
	lda (ptrPepChan),y
	sta valPepTmp1
	iny
	lda (ptrPepChan),y
	sta valPepTmp1+1

	ldy #PEP_CHNDATA_valPrtPer
	lda (ptrPepChan),y
	sta valPepTmp2
	iny
	lda (ptrPepChan),y
	sta valPepTmp2+1

	lda valPepTmp1+1
	cmp valPepTmp2+1
	bcc .LpeppitoChanPorta_less0
	bne .LpeppitoChanPorta_cont0
	lda valPepTmp1
	cmp valPepTmp2
	bcc .LpeppitoChanPorta_less0

	jmp .LpeppitoChanPorta_cont0

.LpeppitoChanPorta_less0:
	clc
	ldy #PEP_CHNDATA_valPrtSpd
	lda valPepTmp1
	adc (ptrPepChan),y
	sta valPepTmp1
	lda valPepTmp1+1
	adc #0x00
	sta valPepTmp1+1

	lda valPepTmp2+1
	cmp valPepTmp1+1
	bcc .LpeppitoChanPorta_less1
	bne .LpeppitoChanPorta_cont0
	lda valPepTmp2
	cmp valPepTmp1
	bcc .LpeppitoChanPorta_less1

	jmp .LpeppitoChanPorta_cont0

.LpeppitoChanPorta_less1:
	lda valPepTmp2
	sta valPepTmp1
	lda valPepTmp2+1
	sta valPepTmp1+1

.LpeppitoChanPorta_cont0:
	lda valPepTmp2+1
	cmp valPepTmp1+1
	bcc .LpeppitoChanPorta_less2
	bne .LpeppitoChanPorta_cont1
	lda valPepTmp2
	cmp valPepTmp1
	bcc .LpeppitoChanPorta_less2

	jmp .LpeppitoChanPorta_cont1

.LpeppitoChanPorta_less2:
	sec
	ldy #PEP_CHNDATA_valPrtSpd
	lda valPepTmp1
	sbc (ptrPepChan),y
	sta valPepTmp1
	lda valPepTmp1+1
	sbc #0x00
	sta valPepTmp1+1

	lda valPepTmp1+1
	cmp valPepTmp2+1
	bcc .LpeppitoChanPorta_less3
	bne .LpeppitoChanPorta_cont1
	lda valPepTmp1
	cmp valPepTmp2
	bcc .LpeppitoChanPorta_less3

	jmp .LpeppitoChanPorta_cont1

.LpeppitoChanPorta_less3:
	lda valPepTmp2
	sta valPepTmp1
	lda valPepTmp2+1
	sta valPepTmp1+1

.LpeppitoChanPorta_cont1:
	ldy #PEP_CHNDATA_valPeriod
	lda valPepTmp1
	sta (ptrPepChan),y
	iny
	lda valPepTmp1+1
	sta (ptrPepChan),y

	rts

peppitoChanVibrato:
	ldy #PEP_CHNDATA_valVtPhs
	lda (ptrPepChan),y
	sta valPepTmp1

	ldy #PEP_CHNDATA_valVibSpd
	lda (ptrPepChan),y
	sta valPepTmp1+1

	__PEP_MUL_MEM8_MEM8 valPepTmp1, valPepTmp1+1

	lda MATH_MULTOUT+2
	sta valPepTmp1
	lda MATH_MULTOUT+3
	sta valPepTmp1+1

	lda valPepTmp1
	and #0x1f
	tax

	lda valPepSin0,x
	sta valPepTmp2

	ldy #PEP_CHNDATA_valVibDep
	lda (ptrPepChan),y

	sta valPepTmp2+1

	__PEP_MUL_MEM8_MEM8 valPepTmp2, valPepTmp2+1

	lda MATH_MULTOUT+2
	sta valPepTmp2
	lda MATH_MULTOUT+3
	sta valPepTmp2+1

	__PEP_DIV_MEM16_IMM16 valPepTmp2, 128

	lda MATH_DIVOUT+4
	sta valPepTmp2

	lda valPepTmp1
	and #0x20
	beq .LpeppitoChanVibrato_update

	sec
	lda #0x01
	sbc valPepTmp2
	sta valPepTmp2

.LpeppitoChanVibrato_update:
	ldy #PEP_CHNDATA_valVibAdd
	lda valPepTmp2
	sta (ptrPepChan),y

	rts

peppitoChanVolSlide:
	lda valPepTmp0+1
	and #0x0f
	sta valPepTmp1

	lda valPepTmp0+1
	lsr
	lsr
	lsr
	lsr

	sec
	sbc valPepTmp1
	sta valPepTmp1

	ldy #PEP_CHNDATA_valVol
	clc
	lda (ptrPepChan),y
	adc valPepTmp1

	bmi .LpeppitoChanVolSlide_setzero

	cmp #0x40
	bcc .LpeppitoChanVolSlide_update

	lda #0x40
.LpeppitoChanVolSlide_update:
	sta (ptrPepChan),y
	rts

.LpeppitoChanVolSlide_setzero:
	lda #0x00
	jmp .LpeppitoChanVolSlide_update

peppitoChanVolUp:
	ldy #PEP_CHNDATA_valVol
	clc
	lda (ptrPepChan),y
	adc valPepTmp0+1

	cmp #0x40
	bcc .LpeppitoChanVolUp_update

	lda #0x40
.LpeppitoChanVolUp_update:
	sta (ptrPepChan),y

	rts

peppitoChanVolDown:
	ldy #PEP_CHNDATA_valVol
	sec
	lda (ptrPepChan),y
	sbc valPepTmp0+1

	bpl .LpeppitoChanVolDown_update

	lda #0x00
.LpeppitoChanVolDown_update:
	sta (ptrPepChan),y

	rts

peppitoChanPortaUp:
	ldy #PEP_CHNDATA_valPeriod
	sec
	lda (ptrPepChan),y
	sbc valPepTmp0+1
	sta (ptrPepChan),y
	iny
	lda (ptrPepChan),y
	sbc #0x00
	sta (ptrPepChan),y

	bmi .LpeppitoChanPortaUp_setzero

	rts

.LpeppitoChanPortaUp_setzero:
	dey
	lda #0x00
	sta (ptrPepChan),y
	iny
	sta (ptrPepChan),y

	rts

peppitoChanPortaDown:
	ldy #PEP_CHNDATA_valPeriod
	clc
	lda (ptrPepChan),y
	adc valPepTmp0+1
	sta (ptrPepChan),y
	iny
	lda (ptrPepChan),y
	adc #0x00
	sta (ptrPepChan),y

	bmi .LpeppitoChanPortaDown_setmax

	rts

.LpeppitoChanPortaDown_setmax:
	dey
	lda #0xff
	sta (ptrPepChan),y
	iny
	lda #0x7f
	sta (ptrPepChan),y

	rts

peppitoChanTick:
;;; Apply tick effects (volume slide, vibrato, portamento, etc.).
	ldy #PEP_CHNDATA_valVtPhs
	lda (ptrPepChan),y
	inc
	sta (ptrPepChan),y

	ldy #PEP_CHNDATA_valFxCnt
	lda (ptrPepChan),y
	inc
	sta (ptrPepChan),y

	ldy #PEP_NOTDATA_valEff

	lda (ptrPepChan),y
	sta valPepTmp0
	iny

	lda (ptrPepChan),y
	sta valPepTmp0+1

	lda valPepTmp0
	cmp #0x0a
	bne .LpeppitoChanTick_tstnext0

	jsr peppitoChanVolSlide
	jsr peppitoChanUpdVol
	rts

.LpeppitoChanTick_tstnext0:
	cmp #0x01
	bne .LpeppitoChanTick_tstnext1

	jsr peppitoChanPortaUp
	jsr peppitoChanUpdFreq
	rts

.LpeppitoChanTick_tstnext1:
	cmp #0x02
	bne .LpeppitoChanTick_tstnext2

	jsr peppitoChanPortaDown
	jsr peppitoChanUpdFreq
	rts

.LpeppitoChanTick_tstnext2:
	cmp #0x04
	bne .LpeppitoChanTick_tstnext3

	jsr peppitoChanVibrato
	jsr peppitoChanUpdFreq
	rts

.LpeppitoChanTick_tstnext3:
	cmp #0x06
	bne .LpeppitoChanTick_tstnext4

	jsr peppitoChanVibrato
	jsr peppitoChanVolSlide

	jsr peppitoChanUpdFreq
	jsr peppitoChanUpdVol

	rts

.LpeppitoChanTick_tstnext4:
	cmp #0x03
	bne .LpeppitoChanTick_tstnext5

	jsr peppitoChanPorta
	jsr peppitoChanUpdFreq

	rts

.LpeppitoChanTick_tstnext5:
	cmp #0x05
	bne .LpeppitoChanTick_tstnext6

	jsr peppitoChanPorta
	jsr peppitoChanVolSlide

	jsr peppitoChanUpdFreq
	jsr peppitoChanUpdVol

	rts

.LpeppitoChanTick_tstnext6:
	cmp #0x19
	bne .LpeppitoChanTick_tstnext7

	ldy #PEP_CHNDATA_valFxCnt
	lda (ptrPepChan),y
	cmp valPepTmp0+1
	bcs .LpeppitoChanTick_retrig0

	rts

.LpeppitoChanTick_retrig0:
	ldy #PEP_CHNDATA_valFxCnt
	lda #0x00
	sta (ptrPepChan),y

	jsr peppitoChanNoteOn
	rts

.LpeppitoChanTick_tstnext7:
	cmp #0x1d
	bne .LpeppitoChanTick_tstnext8

	ldy #PEP_CHNDATA_valFxCnt
	lda (ptrPepChan),y
	cmp valPepTmp0+1
	beq .LpeppitoChanTick_dely0

	rts

.LpeppitoChanTick_dely0:
	jsr peppitoChanTrigger
	rts

.LpeppitoChanTick_tstnext8:
	cmp #0x1c
	bne .LpeppitoChanTick_tstnext9

	ldy #PEP_CHNDATA_valFxCnt
	lda (ptrPepChan),y
	cmp valPepTmp0+1
	beq .LpeppitoChanTick_ntct0

	rts

.LpeppitoChanTick_ntct0:
	ldy #PEP_CHNDATA_valVol
	lda #0x00
	sta (ptrPepChan),y

	jsr peppitoChanUpdVol
	rts

.LpeppitoChanTick_tstnext9:

	rts

pepptioPerformTick:
;;; Apply tick effects on all four channels.
	ldx #0x00
.LpepptioPerformTick_loop1:
;		CPX	#$01
;		BEQ	.LpepptioPerformTick_next
;		CPX	#$02
;		BEQ	.LpepptioPerformTick_next

	phx

	stx valPepChan

	txa
	asl
	tay

	lda idxPepChn0,y
	sta ptrPepChan
	lda idxPepChn0+1,y
	sta ptrPepChan+1

	jsr peppitoChanTick

	plx
.LpepptioPerformTick_next:
	inx
	cpx #0x04
	bne .LpepptioPerformTick_loop1

	rts

peppitoClearChanPLRow:
;;; Clear pattern loop row count on all channels.
	ldx #0x00
.LpeppitoClearChanPLRow_loop1:
;		PHX

	stx valPepChan

	txa
	asl
	tay

	lda idxPepChn0,y
	sta ptrPepChan
	lda idxPepChn0+1,y
	sta ptrPepChan+1

	ldy #PEP_CHNDATA_valPtnLRow
	lda #0x00
	sta (ptrPepChan),y

;		PLX
.LpeppitoClearChanPLRow_next:
	inx
	cpx #0x04
	bne .LpeppitoClearChanPLRow_loop1

	rts

peppitoPerformRow:
;;; Fetch pattern row, trigger notes, and apply row effects.
	lda #0x00

	lda valPepNRow
	bpl .LpeppitoPerformRow_cont0

	ldx cntPepSeqP
	inx
	stx valPepPBrk

	lda #0x00
	sta valPepNRow

.LpeppitoPerformRow_cont0:
	lda valPepPBrk
	bmi .LpeppitoPerformRow_cont1

	cmp valPepSLen
	bcc .LpeppitoPerformRow_more0

	lda #0x00
	sta valPepPBrk
	sta valPepNRow

.LpeppitoPerformRow_more0:
	lda valPepPBrk
	bmi .LpeppitoPerformRow_restart

	cmp cntPepSeqP
	bcc .LpeppitoPerformRow_restart

	jmp .LpeppitoPerformRow_skip0

.LpeppitoPerformRow_restart:
	lda #0x01

	lda valPepSRst
	sta valPepPBrk

.LpeppitoPerformRow_skip0:
	lda valPepPBrk
	sta cntPepSeqP

	jsr peppitoClearChanPLRow

	lda #0xff
	sta valPepPBrk

.LpeppitoPerformRow_cont1:
	lda valPepNRow
	sta cntPepPRow

	inc valPepNRow
	lda #0xc0
	and valPepNRow
	beq .LpeppitoPerformRow_update

	lda #0xff
	sta valPepNRow

.LpeppitoPerformRow_update:
	;; ldz has no base-page form -- `ldz $32` assembles as absolute $0032 --
	;; so it cannot read a page variable. Through A costs the same three bytes.
	lda cntPepSeqP
	taz

	lda [ptrPepMSeq],z

	asl
	asl
	tax

	lda adrPepPtn0+0,x
	sta ptrPepPtrn+0
	lda adrPepPtn0+1,x
	sta ptrPepPtrn+1
	lda adrPepPtn0+2,x
	sta ptrPepPtrn+2
	lda adrPepPtn0+3,x
	sta ptrPepPtrn+3

	lda cntPepPRow
	sta valPepTmp0
	lda #0x00
	sta valPepTmp0+1

	__PEP_ASL_MEM16 valPepTmp0
	__PEP_ASL_MEM16 valPepTmp0
	__PEP_ASL_MEM16 valPepTmp0
	__PEP_ASL_MEM16 valPepTmp0

	__PEP_ADD_PTR32_MEM16 ptrPepPtrn, valPepTmp0

	ldz #0x00
	ldx #0x00
.LpeppitoPerformRow_loop0:
	txa
	asl
	tay

	lda idxPepChn0,y
	sta ptrPepTmpA
	lda idxPepChn0+1,y
	sta ptrPepTmpA+1

	lda [ptrPepPtrn],z
	inz

	sta valPepTmp0

	and #0x0f

	ldy #PEP_NOTDATA_valKey+1
	sta (ptrPepTmpA),y
	dey

	lda [ptrPepPtrn],z
	inz

	sta (ptrPepTmpA),y

	lda valPepTmp0
	and #0x10
	sta valPepTmp0

	lda [ptrPepPtrn],z
	inz

	sta valPepTmp0+1

	and #0xf0
	lsr
	lsr
	lsr
	lsr
	ora valPepTmp0

	beq 1f

1: LDY	#PEP_NOTDATA_valIns
	sta (ptrPepTmpA),y

	lda valPepTmp0+1
	and #0x0f
	sta valPepTmp1

	lda [ptrPepPtrn],z
	inz
	sta valPepTmp1+1

	lda valPepTmp1
	cmp #0x0e
	bne .LpeppitoPerformRow_cont2

	lda valPepTmp1+1
	lsr
	lsr
	lsr
	lsr
	ora #0x10
	sta valPepTmp1

	lda valPepTmp1+1
	and #0x0f
	sta valPepTmp1+1

.LpeppitoPerformRow_cont2:
	lda valPepTmp1
	bne .LpeppitoPerformRow_cont3

	lda valPepTmp1+1
	bmi .LpeppitoPerformRow_cont3
	beq .LpeppitoPerformRow_cont3

	lda #0x0e
	sta valPepTmp1

.LpeppitoPerformRow_cont3:
	ldy #PEP_NOTDATA_valEff
	lda valPepTmp1
	sta (ptrPepTmpA),y

	ldy #PEP_NOTDATA_valPrm
	lda valPepTmp1+1
	sta (ptrPepTmpA),y

	inx
	cpx #0x04
	bne .LpeppitoPerformRow_loop0

	ldx #0x00
.LpeppitoPerformRow_loop1:
;		CPX	#$01
;		BEQ	.LpeppitoPerformRow_next
;		CPX	#$02
;		BEQ	.LpeppitoPerformRow_next

	phx

	stx valPepChan

	txa
	asl
	tay

	lda idxPepChn0,y
	sta ptrPepChan
	lda idxPepChn0+1,y
	sta ptrPepChan+1

	jsr peppitoChanRow

	plx
.LpeppitoPerformRow_next:
	inx
	cpx #0x04
	bne .LpeppitoPerformRow_loop1

	;; Enable audio DMA, through the mixer rather than around it. Upstream set
	;; PCM_BYPASS as well, which feeds the amplifier's PWM inputs from
	;; cpu_pcm_* directly instead of the mixer's headphones_*_out
	;; (audio_complex.vhdl:458-468). xemu models none of that path, so the
	;; difference is audible only on hardware.
	lda #AUDEN
	sta AUDIO_DMA_ENABLE

	rts

peppitoChanRow:
;;; Trigger note or apply row effects for one channel.
	ldy #PEP_NOTDATA_valEff

	lda (ptrPepChan),y
	sta valPepTmp0
	iny

	lda (ptrPepChan),y
	sta valPepTmp0+1

	lda valPepTmp0
	cmp #0x1d
	bne .LpeppitoChanRow_trigger

	lda valPepTmp0+1
	beq .LpeppitoChanRow_trigger

	jmp .LpeppitoChanRow_update

.LpeppitoChanRow_trigger:
	jsr peppitoChanTrigger

.LpeppitoChanRow_update:
#if PEP_SHOW_RASTER
	lda #0x0e
	sta VIC_BORDERCOL
#endif

	jsr peppitoChanTrigEffect

	rts

peppitoMULT10:
;;; Return A * 10.
	asl
	sta valPepTmp4
	asl
	asl
	clc
	adc valPepTmp4
	rts

peppitoChanTrigEffect:
;;; Apply row effect (pattern break, volume set, portamento, etc.).
	ldy #PEP_CHNDATA_valVibAdd
	lda #0x00
	sta (ptrPepChan),y
	ldy #PEP_CHNDATA_valFxCnt
	sta (ptrPepChan),y

	ldy #PEP_NOTDATA_valEff

	lda (ptrPepChan),y
	sta valPepTmp0
	iny
	lda (ptrPepChan),y
	sta valPepTmp0+1

	lda valPepTmp0

	cmp #0x0d
	bne .LpeppitoChanTrigEffect_tstnxt0

;	Pattern break-------------------------------------------
	lda cntPepPtnL
	bmi .LpeppitoChanTrigEffect_pbrk0

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_pbrk0:
	lda valPepPBrk
	bpl .LpeppitoChanTrigEffect_pbrk1

	ldx cntPepSeqP
	inx
	stx valPepPBrk

.LpeppitoChanTrigEffect_pbrk1:
	lda valPepTmp0+1
	lsr
	lsr
	lsr
	lsr
	jsr peppitoMULT10
	sta valPepTmp1

	lda valPepTmp0+1
	and #0x0f

	clc
	adc valPepTmp1
	sta valPepNRow

	cmp #0x40
	bcc .LpeppitoChanTrigEffect_exit

	lda #0x00
	sta valPepNRow

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxt0:
	cmp #0x0f
	bne .LpeppitoChanTrigEffect_tstnxt1

;	Set Speed ----------------------------------------------

	lda valPepTmp0+1
	beq .LpeppitoChanTrigEffect_exit

	cmp #0x20
	bcs .LpeppitoChanTrigEffect_spdch0

	sta valPepSped
	sta cntPepTick

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_spdch0:
;***FIXME:
;		Tempo := Param
	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxt1:
	cmp #0x0c
	bne .LpeppitoChanTrigEffect_tstnxt2

	lda valPepTmp0+1
	cmp #0x40
	bcc .LpeppitoChanTrigEffect_svol0

	lda #0x40

.LpeppitoChanTrigEffect_svol0:
	ldy #PEP_CHNDATA_valVol
	sta (ptrPepChan),y

	jsr peppitoChanUpdVol

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxt2:
	cmp #0x11
	bne .LpeppitoChanTrigEffect_tstnxt3

	jsr peppitoChanPortaUp
	jsr peppitoChanUpdFreq

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxt3:
	cmp #0x12
	bne .LpeppitoChanTrigEffect_tstnxt4

	jsr peppitoChanPortaDown
	jsr peppitoChanUpdFreq

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxt4:
	cmp #0x1a
	bne .LpeppitoChanTrigEffect_tstnxt5

	jsr peppitoChanVolUp
	jsr peppitoChanUpdVol

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxt5:
	cmp #0x1b
	bne .LpeppitoChanTrigEffect_tstnxt6
	jsr peppitoChanVolDown
	jsr peppitoChanUpdVol

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxt6:
	cmp #0x04
	bne .LpeppitoChanTrigEffect_tstnxt7

;	Vibrato ------------------------------------------------

	lda valPepTmp0+1
	lsr
	lsr
	lsr
	lsr

	bne .LpeppitoChanTrigEffect_vibspd0

	jmp .LpeppitoChanTrigEffect_vibcont0

.LpeppitoChanTrigEffect_vibspd0:
	ldy #PEP_CHNDATA_valVibSpd
	sta (ptrPepChan),y

.LpeppitoChanTrigEffect_vibcont0:
	lda valPepTmp0+1
	and #0x0f

	bne .LpeppitoChanTrigEffect_vibdep0

	jmp .LpeppitoChanTrigEffect_vibcont1

.LpeppitoChanTrigEffect_vibdep0:
	ldy #PEP_CHNDATA_valVibDep
	sta (ptrPepChan),y

.LpeppitoChanTrigEffect_vibcont1:
	jsr peppitoChanVibrato
	jsr peppitoChanUpdFreq

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxt7:
	cmp #0x06
	bne .LpeppitoChanTrigEffect_tstnxt8

	jsr peppitoChanVibrato
	jsr peppitoChanUpdFreq

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxt8:
	cmp #0x03
	bne .LpeppitoChanTrigEffect_tstnxt9

	lda valPepTmp0+1
	bne .LpeppitoChanTrigEffect_tport0

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tport0:
	ldy #PEP_CHNDATA_valPrtSpd
	sta (ptrPepChan),y

	jsr peppitoChanUpdFreq
	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxt9:
	cmp #0x0b
	bne .LpeppitoChanTrigEffect_tstnxta

	lda cntPepPtnL
	bpl .LpeppitoChanTrigEffect_exit

	lda valPepTmp0+1
	sta valPepPBrk
	lda #0x00
	sta valPepNRow

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxta:
	cmp #0x16
	bne .LpeppitoChanTrigEffect_tstnxtb

	ldy #PEP_CHNDATA_valPtnLRow

	lda valPepTmp0+1
	bne .LpeppitoChanTrigEffect_ptnl0

	lda cntPepPRow
	sta (ptrPepChan),y

.LpeppitoChanTrigEffect_ptnl0:
	lda (ptrPepChan),y
	cmp cntPepPRow
	bcs .LpeppitoChanTrigEffect_exit

	lda valPepPBrk
	bpl .LpeppitoChanTrigEffect_exit

	lda cntPepPtnL
	bpl .LpeppitoChanTrigEffect_ptnl1

	lda valPepTmp0+1
	sta cntPepPtnL

	lda valPepChan
	sta valPepPLCh

.LpeppitoChanTrigEffect_ptnl1:
	lda valPepChan
	cmp valPepPLCh
	bne .LpeppitoChanTrigEffect_exit

	lda cntPepPtnL
	bne .LpeppitoChanTrigEffect_ptnl2

	ldx cntPepPRow
	inx
	txa
	sta (ptrPepChan),y

	jmp .LpeppitoChanTrigEffect_ptnl3

.LpeppitoChanTrigEffect_ptnl2:
	lda (ptrPepChan),y
	sta valPepNRow

.LpeppitoChanTrigEffect_ptnl3:
	dec cntPepPtnL

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_tstnxtb:
	cmp #0x1c
	bne .LpeppitoChanTrigEffect_exit

	lda valPepTmp0+1
	bmi .LpeppitoChanTrigEffect_ntct0
	beq .LpeppitoChanTrigEffect_ntct0

	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_ntct0:
	ldy #PEP_CHNDATA_valVol
	lda #0x00
	sta (ptrPepChan),y

	jsr peppitoChanUpdVol
	jmp .LpeppitoChanTrigEffect_exit

.LpeppitoChanTrigEffect_exit:
	rts

peppitoChanUpdFreq:
;;; Update hardware frequency register; skip if channel >= MUSIC_CHANNELS.
	lda valPepChan
	cmp #MUSIC_CHANNELS
	bcc 1f
	rts
1:
	jsr peppitoChanCalcFreq

	lda valPepChan
	asl
	asl
	asl
	asl
	tax

;	Rate
	lda MATH_MULTOUT+2
	sta AUDIO_CH0+CHXFREQ,x
	lda MATH_MULTOUT+3
	sta AUDIO_CH0+CHXFREQ+1,x
	lda #0x00
	sta AUDIO_CH0+CHXFREQ+2,x

	rts

peppitoChanUpdVol:
;;; Update hardware volume; module volume (0-64) maps directly to CHXVOLUME
;;; (0-255). Skip if channel >= MUSIC_CHANNELS.
	lda valPepChan
	cmp #MUSIC_CHANNELS
	bcc 1f
	rts
1:
	asl
	asl
	asl
	asl
	tax

	ldy #PEP_CHNDATA_valVol
	lda (ptrPepChan),y
	sta AUDIO_CH0+CHXVOLUME,x

	lda #0x00
	ldx valPepChan
	sta CH0RVOL,x


	rts

peppitoChanTrigger:
;;; Load instrument, calculate period, set sample pointers.
	ldy #PEP_NOTDATA_valIns
	lda (ptrPepChan),y
	sta valPepTmp0

	beq .LpeppitoChanTrigger_cont0

	ldy #PEP_CHNDATA_valAssIns
	sta (ptrPepChan),y

/*
	pha  ; DETECT SAMPLE BEAT!!!
	asl
	asl
	clc
	adc #0x08
	sta 0xc000
	pla
*/

	asl
	tax
	lda idxPepIns0,x
	sta ptrPepTmpA

	lda idxPepIns0+1,x
	sta ptrPepTmpA+1

	ldy #PEP_CHNDATA_valSmpOffs
	lda #0x00
	sta (ptrPepChan),y
	iny
	sta (ptrPepChan),y

	ldy #PEP_CHNDATA_valFTune
	ldz #PEP_INSDATA_valFTune

	lda (ptrPepTmpA),z
	sta (ptrPepChan),y

	ldy #PEP_CHNDATA_valVol
	ldz #PEP_INSDATA_valVol

	lda (ptrPepTmpA),z
	sta (ptrPepChan),y

;	If (Instruments[ Ins ].LoopLength > 0) And ( Channel.Instrument > 0 ) Then
	ldy #PEP_INSDATA_valLLen
	lda (ptrPepTmpA),y
	iny
	ora (ptrPepTmpA),y
	beq .LpeppitoChanTrigger_cont0

	ldy #PEP_CHNDATA_valSelIns
	lda (ptrPepChan),y
	beq .LpeppitoChanTrigger_cont0

	ldy #PEP_CHNDATA_valSelIns
	lda valPepTmp0
	sta (ptrPepChan),y

.LpeppitoChanTrigger_cont0:
;***FIXME:
;	If Channel.Note.Effect = $9 Then Begin
;		Channel.SampleOffset := ( Channel.Note.Param And $FF ) Shl 8;
;	End Else If Channel.Note.Effect = $15 Then Begin
;		Channel.FineTune := Channel.Note.Param;
;	End;

	ldy #PEP_NOTDATA_valKey
	lda (ptrPepChan),y
	sta valPepTmp0
	iny
	ora (ptrPepChan),y
	beq .LpeppitoChanTrigger_done

	lda (ptrPepChan),y
	sta valPepTmp0+1

;		Period := ( Channel.Note.Key * FineTuning[ Channel.FineTune And $F ] ) Shr 11;
	ldy #PEP_CHNDATA_valFTune
	lda (ptrPepChan),y
	and #0x0f
	asl
	tax
	lda valPepFTn0,x
	sta valPepTmp2
	lda valPepFTn0+1,x
	sta valPepTmp2+1

	__PEP_MUL_MEM16_MEM16 valPepTmp0, valPepTmp2

	lda MATH_MULTOUT
	sta valPepTmp1
	lda MATH_MULTOUT+1
	sta valPepTmp1+1
	lda MATH_MULTOUT+2
	sta valPepTmp1+2
	lda MATH_MULTOUT+3
	sta valPepTmp1+3

	__PEP_DIV_MEM32_IMM16 valPepTmp1, 2048

	lda MATH_DIVOUT+4
	sta valPepTmp0
	lda MATH_DIVOUT+5
	sta valPepTmp0+1

;		Channel.PortaPeriod := ( Period Shr 1 ) + ( Period And 1 );
	lda valPepTmp0
	sta valPepTmp1
	lda valPepTmp0+1
	sta valPepTmp1+1

	__PEP_LSR_MEM16 valPepTmp1

	clc
	lda valPepTmp0
	and #0x01
	adc valPepTmp1
	sta valPepTmp1
	lda #0x00
	adc valPepTmp1+1
	sta valPepTmp1+1

	ldy #PEP_CHNDATA_valPrtPer
	lda valPepTmp1
	sta (ptrPepChan),y
	iny
	lda valPepTmp1+1
	sta (ptrPepChan),y

	ldy #PEP_NOTDATA_valEff
	lda (ptrPepChan),y
	cmp #0x03
	beq .LpeppitoChanTrigger_done
	cmp #0x05
	beq .LpeppitoChanTrigger_done

	ldy #PEP_CHNDATA_valAssIns
	lda (ptrPepChan),y
	ldy #PEP_CHNDATA_valSelIns
	sta (ptrPepChan),y

;		Channel.Period:= Channel.PortaPeriod
	ldy #PEP_CHNDATA_valPeriod
	lda valPepTmp1
	sta (ptrPepChan),y
	iny
	lda valPepTmp1+1
	sta (ptrPepChan),y

	ldy #PEP_CHNDATA_valVtPhs
	lda #0x00
	sta (ptrPepChan),y

	jsr peppitoChanNoteOn

.LpeppitoChanTrigger_done:
	rts

peppitoChanCalcFreq:
;;; Convert period (with vibrato offset) to MEGA65 sample rate.
	ldy #PEP_CHNDATA_valVibAdd
	lda (ptrPepChan),y
	sta valPepTmp1

	bpl .LpeppitoChanCalcFreq_pos

	lda #0xff
	sta valPepTmp1+1

	jmp .LpeppitoChanCalcFreq_cont0

.LpeppitoChanCalcFreq_pos:
	lda #0x00
	sta valPepTmp1+1

.LpeppitoChanCalcFreq_cont0:
	ldy #PEP_CHNDATA_valPeriod
	clc
	lda (ptrPepChan),y
	adc valPepTmp1
	sta valPepTmp1
	iny
	lda (ptrPepChan),y
	adc valPepTmp1+1
	sta valPepTmp1+1

	bne .LpeppitoChanCalcFreq_cont1

	lda valPepTmp1

	cmp #0x0e
	bcs .LpeppitoChanCalcFreq_cont1

	lda #<6848
	sta valPepTmp1
	lda #>6848
	sta valPepTmp1+1

.LpeppitoChanCalcFreq_cont1:
;	Amiga frequency
	lda #<VAL_FAC_AMIGARATEL
	sta MATH_MULTINA
	lda #>VAL_FAC_AMIGARATEL
	sta MATH_MULTINA+1
	lda #<VAL_FAC_AMIGARATEH
	sta MATH_MULTINA+2
	lda #>VAL_FAC_AMIGARATEH
	sta MATH_MULTINA+3

	lda valPepTmp1
	sta MATH_MULTINB
	lda valPepTmp1+1
	sta MATH_MULTINB+1
	lda #0x00
	sta MATH_MULTINB+2
	sta MATH_MULTINB+3

	__PEP_MATH_SETTLE

	lda MATH_DIVOUT+4
	sta valPepTmp0
	lda MATH_DIVOUT+5
	sta valPepTmp0+1
	lda MATH_DIVOUT+6
	sta valPepTmp0+2
	lda MATH_DIVOUT+7
	sta valPepTmp0+3

;	M65 frequency
	lda #<VAL_FAC_M65RATERTL
	sta MATH_MULTINA
	lda #>VAL_FAC_M65RATERTL
	sta MATH_MULTINA+1
	lda #<VAL_FAC_M65RATERTH
	sta MATH_MULTINA+2
	lda #>VAL_FAC_M65RATERTH
	sta MATH_MULTINA+3

	lda valPepTmp0
	sta MATH_MULTINB
	lda valPepTmp0+1
	sta MATH_MULTINB+1
	lda valPepTmp0+2
	sta MATH_MULTINB+2
	lda valPepTmp0+3
	sta MATH_MULTINB+3

	rts

peppitoChanNoteOn:
;;; Set sample address and loop, start playback on the channel.
	lda valPepChan
	cmp #MUSIC_CHANNELS
	bcc 1f
	rts
1:
;		BNE	.LpeppitoChanNoteOn_start
;
;
;.LpeppitoChanNoteOn_start:

	asl
	asl
	asl
	asl
	sta valPepTmp2

	jsr peppitoChanCalcFreq

	ldx valPepTmp2

;	Silence
	lda #0x00
	sta AUDIO_CH0+CHXFLAGS,x

	ldy #PEP_CHNDATA_valSelIns
	lda (ptrPepChan),y

	asl
	tay

	lda idxPepIns0,y
	sta ptrPepTmpA
	lda idxPepIns0+1,y
	sta ptrPepTmpA+1

	lda MATH_MULTOUT+2
	sta AUDIO_CH0+CHXFREQ,x
	lda MATH_MULTOUT+3
	sta AUDIO_CH0+CHXFREQ+1,x
	lda #0x00
	sta AUDIO_CH0+CHXFREQ+2,x

	ldy #PEP_INSDATA_valLLen
	lda (ptrPepTmpA),y
	iny
	ora (ptrPepTmpA),y
	beq .LpeppitoChanNoteOn_noloop

	ldy #PEP_INSDATA_valLStrt
	lda (ptrPepTmpA),y
	sta valPepTmp0
	iny
	lda (ptrPepTmpA),y
	sta valPepTmp0+1

	ldy #PEP_INSDATA_ptrSmpD
	clc
	lda (ptrPepTmpA),y
	adc valPepTmp0
	sta valPepTmp3
	sta AUDIO_CH0+CHXBADDR,x
	iny
	lda (ptrPepTmpA),y
	adc valPepTmp0+1
	sta valPepTmp3+1
	sta AUDIO_CH0+CHXBADDR+1,x
	iny
	lda (ptrPepTmpA),y
	adc #0x00
	sta valPepTmp3+2
	sta AUDIO_CH0+CHXBADDR+2,x
;		INY

;***FIXME:
;	Should include PEP_CHNDATA_valSmpOffs

	ldy #PEP_INSDATA_ptrSmpD
	lda (ptrPepTmpA),y
	sta AUDIO_CH0+CHXCURADDR,x
	iny
	lda (ptrPepTmpA),y
	sta AUDIO_CH0+CHXCURADDR+1,x
	iny
	lda (ptrPepTmpA),y
	sta AUDIO_CH0+CHXCURADDR+2,x

	sec
	lda valPepTmp3
	sbc #0x01
	sta valPepTmp3
	lda valPepTmp3+1
	sbc #0x00
	sta valPepTmp3+1

	ldy #PEP_INSDATA_valLLen
	clc
	lda (ptrPepTmpA),y
	adc valPepTmp3
	sta AUDIO_CH0+CHXTADDR,x
	iny
	lda (ptrPepTmpA),y
	adc valPepTmp3+1
	sta AUDIO_CH0+CHXTADDR+1,x

	lda #%11000010

	sta AUDIO_CH0+CHXFLAGS,x

	jmp .LpeppitoChanNoteOn_finish

.LpeppitoChanNoteOn_noloop:
	ldy #PEP_INSDATA_ptrSmpD
	lda (ptrPepTmpA),y
	sta valPepTmp3
	sta AUDIO_CH0+CHXBADDR,x
	iny
	lda (ptrPepTmpA),y
	sta valPepTmp3+1
	sta AUDIO_CH0+CHXBADDR+1,x
	iny
	lda (ptrPepTmpA),y
	sta valPepTmp3+2
	sta AUDIO_CH0+CHXBADDR+2,x
	iny

	ldy #PEP_CHNDATA_valSmpOffs
	clc
	lda (ptrPepChan),y
	adc valPepTmp3
	sta AUDIO_CH0+CHXCURADDR,x
	iny
	lda (ptrPepChan),y
	adc valPepTmp3+1
	sta AUDIO_CH0+CHXCURADDR+1,x
	lda #0x00
	adc valPepTmp3+2
	sta AUDIO_CH0+CHXCURADDR+2,x

	ldy #PEP_INSDATA_valSLen
	clc
	lda (ptrPepTmpA),y
	adc valPepTmp3
	sta AUDIO_CH0+CHXTADDR,x
	iny
	lda (ptrPepTmpA),y
	adc valPepTmp3+1
	sta AUDIO_CH0+CHXTADDR+1,x

	lda #%10000010
	sta AUDIO_CH0+CHXFLAGS,x

.LpeppitoChanNoteOn_finish:
	;; A module's volume is 0-64 against CHXVOLUME's 0-255 (the Book, "Sample
	;; Playback frequency and Volume"), taken as it is. A side sums its
	;; channels and clips (gs4510.vhdl:4211-4227): the music's pair fills half
	;; of it, and speech on channel 3 the rest -- doubled, the music buried it.
	ldy #PEP_CHNDATA_valVol
	lda (ptrPepChan),y
	sta AUDIO_CH0+CHXVOLUME,x

	lda #0x00
	ldx valPepChan
	sta CH0RVOL,x


	rts


;;; ---------------------------------------------------------------------------
;;; State.  Split by what it starts as: the zero blocks are filled from the
;;; module before anything reads them, so they cost nothing in the image.
;;; ---------------------------------------------------------------------------

	.section .data.pep,"aw",@progbits

	;; Offsets of the instrument and channel records, so a record can be
	;; reached by index without a multiply.
idxPepIns0:
	.set pep_ins, 0
	.rept 32
	.byte (recPepIns0+PEP_INSDATA_size * pep_ins)@mos16lo
	.byte (recPepIns0+PEP_INSDATA_size * pep_ins)@mos16hi
	.set pep_ins, pep_ins+1
	.endr

idxPepChn0:
	.set pep_chn, 0
	.rept 4
	.byte (recPepChn0+PEP_CHNDATA_size * pep_chn)@mos16lo
	.byte (recPepChn0+PEP_CHNDATA_size * pep_chn)@mos16hi
	.set pep_chn, pep_chn+1
	.endr

	;; The four module signatures this driver will play.
valPepSig0:
	.byte 0x4d, 0x2e, 0x4b, 0x2e  ; M.K.
	.byte 0x4d, 0x21, 0x4b, 0x21  ; M!K!
	.byte 0x46, 0x4c, 0x54, 0x34  ; FLT4
	.byte 0x46, 0x4c, 0x54, 0x38  ; FLT8

	;; A quarter sine, mirrored in code, for the vibrato.
valPepSin0:
	.byte 0,  24,  49,  74,  97, 120, 141, 161
	.byte 180, 197, 212, 224, 235, 244, 250, 253
	.byte 255, 253, 250, 244, 235, 224, 212, 197
	.byte 180, 161, 141, 120,  97,  74,  49,  24

	;; Period of the top note at each of the sixteen finetune settings.
valPepFTn0:
	.2byte 4340, 4308, 4277, 4247, 4216, 4186, 4156, 4126
	.2byte 4096, 4067, 4037, 4008, 3979, 3951, 3922, 3894

	.section .rodata.pep,"a",@progbits

	.section .bss.pep,"aw",@nobits

#if PEP_SHOW_RASTER
valPepRstr:
	.byte 0
#endif

	;; One 32-bit Attic pointer per pattern, and its index, both written by
	;; peppitoReadSeq before any read.
adrPepPtn0:
	.zero 128 * 4
idxPepPtn0:
	.zero 128

recPepIns0:
	.zero 32 * PEP_INSDATA_size
recPepChn0:
	.zero 4 * PEP_CHNDATA_size


	;; The driver's zero page, moved here with TAB so it does not collide
	;; with the compiler's.  Must be its own 256-byte window and nothing else.
	;;
	;; adrPepMODL is a page offset to the driver; C++ writes the same four
	;; bytes at their absolute address, which is what pep_mod_addr names.
	.balign 256
	.global pep_page
pep_page:
	.zero 256

	.global pep_mod_addr
	.set pep_mod_addr, pep_page + adrPepMODL
