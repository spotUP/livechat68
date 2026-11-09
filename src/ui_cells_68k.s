; 68000 hot path for ui_cells.c (vbcc +aos68k ABI: arguments on the stack, result in d0,
; d0-d1/a0-a1 scratch). Built only into the Amiga library; every other target uses the C in
; ui_cells.c, which is the reference for what this must do.
;
; long ui_cells_first_diff(const ui_cell *a, const ui_cell *b, long n)
;   Index of the first cell where a and b differ, or n when all n cells are equal. A cell is
;   4 bytes and both arrays are long-aligned (malloc), so one cmpm.l compares one cell. The
;   flush calls this once per run of unchanged cells, so a redraw that changed three cells
;   costs about two instructions per cell instead of a dozen.

	section	text,code
	xdef	_ui_cells_first_diff

_ui_cells_first_diff:
	move.l	d2,-(sp)
	move.l	8(sp),a0		; a
	move.l	12(sp),a1		; b
	move.l	16(sp),d1		; cells left to compare
	move.l	a0,d0			; start of a: the index comes from how far a0 moved
.chunk:
	move.l	d1,d2			; dbne counts 16 bits: compare at most $8000 cells per pass
	cmpi.l	#$8000,d2
	bls.s	.run
	move.l	#$8000,d2
.run:
	tst.l	d2
	beq.s	.same			; nothing left: every cell matched
	sub.l	d2,d1
	subq.l	#1,d2			; dbne stops at -1
.loop:
	cmpm.l	(a0)+,(a1)+
	dbne	d2,.loop
	beq.s	.chunk			; Z from the last cmpm: the whole pass matched
	move.l	a0,d1			; a0 is one cell past the difference
	sub.l	d0,d1
	lsr.l	#2,d1
	subq.l	#1,d1
	move.l	d1,d0
	move.l	(sp)+,d2
	rts
.same:
	move.l	16(sp),d0		; n
	move.l	(sp)+,d2
	rts
