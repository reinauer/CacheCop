| CacheCop entry stubs for CachePreDMA()/CachePostDMA().
|
| exec ABI for both functions (see inline/exec.h, LVO -762 / -768):
|     a0 = APTR    vaddress
|     a1 = ULONG * &length   (CachePreDMA may shorten *length)
|     d0 = ULONG   flags
|     a6 = struct ExecBase *
| CachePreDMA returns the physical address in d0.
|
| The stubs pass the caller's registers through to the original vectors
| untouched and hand the C hooks a pointer to a DMAFrame on the stack:
|
|     struct DMAFrame {
|         ULONG  flags;    0(frame)
|         APTR   addr;     4(frame)
|         ULONG *lenp;     8(frame)
|         ULONG  len;     12(frame)  *lenp at entry (0 if lenp == NULL)
|         APTR   pc;      16(frame)  caller's return address
|     };
|
| The C hooks follow the normal m68k C ABI (args on the stack, d0/d1/a0/a1
| scratch), so any register the caller might still care about is either
| restored from the frame or was scratch under the library ABI anyway.

        .text
        .even

| g_InFlight counts callers currently executing stub code (including the
| original CachePreDMA() called from _cc_PreStub, which returns into it).
| addq/subq on memory are single instructions, so they are atomic with
| respect to interrupts on a single 68k CPU.

        .globl  _cc_PreStub
_cc_PreStub:
        addq.l  #1,_g_InFlight
        move.l  a1,d1                   | d1 is scratch for CachePreDMA
        beq.s   .Lpre_nolen
        move.l  (a1),d1
.Lpre_nolen:
        move.l  d1,-(sp)                | frame.len (requested length)
        movem.l d0/a0-a1,-(sp)          | frame.flags, frame.addr, frame.lenp
        pea     .Lpre_ret(pc)
        move.l  _g_OldPre,-(sp)
        rts                             | call original with a0/a1/d0/a6 intact
.Lpre_ret:
        move.l  d0,-(sp)                | keep physical address
        move.l  d0,-(sp)                | arg 2: phys
        pea     8(sp)                   | arg 1: &frame
        jsr     _cc_pre_hook
        addq.l  #8,sp
        move.l  (sp)+,d0
        lea     16(sp),sp               | drop frame, leaving caller's PC
        subq.l  #1,_g_InFlight
        rts

        .globl  _cc_PostStub
_cc_PostStub:
        addq.l  #1,_g_InFlight
        move.l  a1,d1
        beq.s   .Lpost_nolen
        move.l  (a1),d1
.Lpost_nolen:
        move.l  d1,-(sp)                | frame.len
        movem.l d0/a0-a1,-(sp)          | frame.flags, frame.addr, frame.lenp
        move.l  sp,-(sp)                | arg 1: &frame
        jsr     _cc_post_hook
        addq.l  #4,sp
        movem.l (sp)+,d0/a0-a1          | restore original arguments
        addq.l  #4,sp                   | drop frame.len
        move.l  _g_OldPost,-(sp)
        subq.l  #1,_g_InFlight          | the original returns to our caller
        rts                             | tail-jump to original
