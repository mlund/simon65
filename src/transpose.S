; Four bitplanes into four-bit glyph bytes: the figure decoder's transpose.
;
; extern "C" void chunky16_packed4(const uint16_t *word, uint8_t *out)
;
; The four-plane case of chunky16_packed (planar.hpp), which stays the
; reference: the host and machine must agree on the output. One line of
; sixteen pixels: word[0..3] are the planes, most significant bit the leftmost
; pixel; out gets eight bytes, two pixels each, the low nybble the left pixel.
;
; Each output byte is two PACK lookups (planar.hpp): one for planes 0 and 1,
; one for planes 2 and 3 shifted up two bits. A lookup takes four bits of each
; of its two planes -- the same four pixels -- so a plane byte's nybbles are
; paired across planes before indexing.
;
; In C++ this was about 1,500 cycles a line; most of it went on the plane
; words shifted in place and the general nybble arithmetic. Here the bytes are
; read where they lie.
;
; ABI (llvm-mos): word in __rs1, out in __rs2. A leaf: A, X, Y and
; __rc2-__rc10 only, all caller-saved. Z is the output index, nought on entry
; and put back to nought before returning (compiled code assumes it).

#include "banknumbers.hpp"

	.section BANK_SECTION(AGOS_ROOM_BANK),"ax",@progbits
	.globl	chunky16_packed4
	.type	chunky16_packed4,@function

#define WORD __rc2
#define OUT __rc4
; Scratch named through mos8(): the imaginary registers resolve only at link
; time, and the assembler would otherwise pick the three-byte absolute form.
#define P0 mos8(__rc6)
#define P1 mos8(__rc7)
#define P2 mos8(__rc8)
#define P3 mos8(__rc9)
#define LOW mos8(__rc10)
#define PACK0 mos16(PACK_TABLE)
#define PACK1 mos16(PACK_TABLE+256)

; One output byte from the pair indices in X (planes 0, 1) and Y (planes 2,
; 3), through table T, stored and the output index stepped: inz is one cycle
; where inw on the pointer is seven (mega65-user-guide 4510.cycles), sixteen
; times a line.
.macro emit t
	lda	\t,y
	asl
	asl
	ora	\t,x
	sta	(OUT),z
	inz
.endm

; Eight pixels: the plane bytes at offset \o of each word (1, the high byte,
; for the left eight; 0 for the right eight).
.macro half o
	ldy	#\o
	lda	(WORD),y
	sta	P0
	ldy	#\o+2
	lda	(WORD),y
	sta	P1
	ldy	#\o+4
	lda	(WORD),y
	sta	P2
	ldy	#\o+6
	lda	(WORD),y
	sta	P3

	; Left four pixels: the high nybbles, paired.
	lda	P1
	lsr
	lsr
	lsr
	lsr
	sta	LOW
	lda	P0
	and	#$f0
	ora	LOW
	tax
	lda	P3
	lsr
	lsr
	lsr
	lsr
	sta	LOW
	lda	P2
	and	#$f0
	ora	LOW
	tay
	emit	PACK0
	emit	PACK1

	; Right four pixels: the low nybbles, paired.
	lda	P1
	and	#$0f
	sta	LOW
	lda	P0
	asl
	asl
	asl
	asl
	ora	LOW
	tax
	lda	P3
	and	#$0f
	sta	LOW
	lda	P2
	asl
	asl
	asl
	asl
	ora	LOW
	tay
	emit	PACK0
	emit	PACK1
.endm

chunky16_packed4:
	half	1
	half	0
	ldz	#0
	rts
	.size	chunky16_packed4, .-chunky16_packed4
