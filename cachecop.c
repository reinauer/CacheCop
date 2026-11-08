/*
 * CacheCop — CachePreDMA()/CachePostDMA() auditor for AmigaOS
 *
 * Patches both exec vectors and tracks every DMA transfer from its first
 * CachePreDMA() (the one without DMA_Continue) to the CachePostDMA() that
 * closes it. "cachecop status" lists the transfers that are still
 * outstanding, with the caller's PC, task and age, so a leaking driver can
 * be identified. Controlled through a public message port.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <libraries/configvars.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>
#define EXPANSION_BASE_NAME g_ExpansionBase
#include <proto/expansion.h>

#include <string.h>
#include <stdarg.h>

/* The Makefile passes the git version and build date */
#ifndef CC_VERSION
#define CC_VERSION "dev"
#endif
#ifndef CC_DATE
#define CC_DATE "1.1.2026"
#endif

/*
 * DMA_Continue, DMA_NoModify and DMA_ReadFromRAM come from exec/execbase.h
 * (V37+). DMA_Continue is (1L<<1); 0x4 is DMA_NoModify.
 */
#ifndef DMA_Continue
#error "DMA_Continue not defined: point NDK= at an NDK 3.1+ include directory"
#endif

/* exec.library LVOs (see inline/exec.h: jsr a6@(-762) / a6@(-768)) */
#define LVOCachePreDMA  (-762)
#define LVOCachePostDMA (-768)

extern struct ExecBase *SysBase;

static const char kPortName[] = "CacheCop";
static const char kVersion[] __attribute__((used)) =
    "$VER: CacheCop " CC_VERSION " (" CC_DATE ")";

/* ------------------------------------------------------------------ */
/* Shared state (written by the hooks, any context)                   */
/* ------------------------------------------------------------------ */

#define CC_MAX_XFERS 128
#define CC_NAMELEN   24

#define CC_WHERESIZE 64

/* Where a call came from */
struct Caller {
    APTR         pc;       /* return address of the CachePre/PostDMA() call */
    struct Task *task;     /* SysBase->ThisTask at the time */
    UBYTE        inint;    /* stack pointer was outside the task's stack */
    UBYTE        resolved; /* where[] is filled in (empty if unknown) */
    char         name[CC_NAMELEN];  /* task or CLI command name */
    /* What code pc belongs to. The hooks can't look this up (they may run
       in an interrupt), so the daemon fills it in soon after, while the
       code is still loaded. */
    char         where[CC_WHERESIZE];
};

/* One DMA transfer, from its first CachePreDMA() to its CachePostDMA() */
struct Xfer {
    UBYTE  used;
    UBYTE  stale;          /* already reported as outstanding too long */
    UWORD  chunks;         /* CachePreDMA() calls, including the first */
    APTR   start;          /* vaddress of the first call */
    ULONG  reqlen;         /* length requested by the first call */
    ULONG  covered;        /* bytes prepared so far, contiguous from start;
                              exceeds reqlen when the driver splits the
                              transfer itself and continues it */
    ULONG  flags;          /* flags of the first call */
    ULONG  t0;             /* g_Now when the transfer started */
    struct Caller who;     /* caller of the first call */
};

/* Violations, reported as they happen */
enum {
    EV_NONE,
    EV_POST_UNMATCHED,     /* CachePostDMA() that matches no transfer */
    EV_CONT_UNMATCHED,     /* DMA_Continue that matches no transfer */
    EV_OVERLAP,            /* new transfer overlaps an outstanding one */
    EV_DIRECTION,          /* DMA_ReadFromRAM differs between pre and post */
    EV_SHORT_IGNORED,      /* shortened grant posted in full, no continue */
    EV_LENGTH,             /* post length isn't the initial length */
    EV_INSIDE,             /* post address isn't the initial address */
    EV_TABLE_FULL,         /* transfer table overflowed */
    EV_STALE,              /* outstanding longer than TIMEOUT seconds */
    EV_POST_CONTINUE,      /* DMA_Continue passed to CachePostDMA() */
    EV_COUNT
};

static const char *const kEventNames[EV_COUNT] = {
    "",
    "CachePostDMA() without CachePreDMA()",
    "DMA_Continue without a first CachePreDMA()",
    "CachePreDMA() overlaps an outstanding transfer",
    "DMA_ReadFromRAM differs between pre and post",
    "posted past a shortened grant without DMA_Continue",
    "post length is not the transfer's initial length",
    "post address is not the transfer's initial address",
    "transfer table full, later starts untracked",
    "transfer outstanding longer than TIMEOUT",
    "DMA_Continue passed to CachePostDMA()",
};

struct Event {
    UBYTE  type;
    UBYTE  hasXfer;        /* xfer below is valid */
    UWORD  pad;
    ULONG  t;              /* g_Now */
    APTR   addr;           /* the offending call's arguments */
    ULONG  len;
    ULONG  flags;
    struct Caller who;     /* the offending call */
    struct Xfer xfer;      /* the transfer it relates to */
};

#define CC_MAX_EVENTS 64

struct Stats {
    LONG  active;          /* transfers currently in the table */
    ULONG maxActive;
    ULONG starts;          /* CachePreDMA() without DMA_Continue */
    ULONG continues;       /* CachePreDMA() with DMA_Continue */
    ULONG posts;           /* CachePostDMA() */
    ULONG untracked;       /* starts dropped because the table was full */
    ULONG events[EV_COUNT];
};

static struct Xfer  g_Xfers[CC_MAX_XFERS];
static struct Stats g_Stats;

/* Event ring: entry n lives at g_Events[n % CC_MAX_EVENTS] */
static struct Event g_Events[CC_MAX_EVENTS];
static ULONG g_EvSeq;                  /* events ever emitted */

/* Who to Signal() when an event is emitted */
static struct Task *g_DaemonTask;
static ULONG g_EvSigMask;

/* Options */
static ULONG g_Timeout = 30;   /* seconds; 0 = don't watch */

/* Seconds since "cachecop start", advanced by the daemon's timer */
static volatile ULONG g_Now;

/* Old function vectors (referenced from stubs.s) */
APTR g_OldPre = NULL;
APTR g_OldPost = NULL;

/* Number of callers currently inside the stubs (maintained by stubs.s) */
volatile LONG g_InFlight = 0;

/* ------------------------------------------------------------------ */
/* Hooks (called from stubs.s, possibly from interrupts)              */
/* ------------------------------------------------------------------ */

/*
 * Entry stubs live in stubs.s. They pass a0/a1/d0/a6 through to the original
 * vectors untouched and call the hooks below with a pointer to this frame.
 * Keep the layout in sync with stubs.s.
 */
struct DMAFrame {
    ULONG  flags;   /* d0 */
    APTR   addr;    /* a0 */
    ULONG *lenp;    /* a1 */
    ULONG  len;     /* *lenp at entry */
    APTR   pc;      /* caller's return address */
};

extern UBYTE cc_PreStub[], cc_PostStub[];
void cc_pre_hook(struct DMAFrame *f, APTR phys);
void cc_post_hook(struct DMAFrame *f);

/*
 * The hooks may be called from interrupt servers, where Forbid() offers no
 * protection, so shared state is only touched under Disable(). Keep that
 * short, and don't call anything here that isn't safe from an interrupt.
 */

static void copy_name(char *dst, const char *src, ULONG len)
{
    ULONG i;

    if (!src)
        len = 0;
    if (len > CC_NAMELEN - 1)
        len = CC_NAMELEN - 1;
    for (i = 0; i < len && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void capture_caller(struct Caller *c, APTR pc, APTR sp)
{
    struct Task *t = SysBase->ThisTask;

    c->pc = pc;
    c->task = t;
    c->name[0] = '\0';
    c->resolved = 0;
    c->where[0] = '\0';
    if (!t) {
        c->inint = 1;
        return;
    }
    /*
     * There's no way to read SR from user mode on a 68010+, so guess: an
     * interrupt runs on the supervisor stack, outside the interrupted
     * task's stack bounds. (StackSwap() users can trip this too.)
     */
    c->inint = (UBYTE *)sp < (UBYTE *)t->tc_SPLower ||
               (UBYTE *)sp >= (UBYTE *)t->tc_SPUpper;

    /* For shell commands, the command name says more than "Shell Process" */
    if (t->tc_Node.ln_Type == NT_PROCESS) {
        struct Process *pr = (struct Process *)t;
        struct CommandLineInterface *cli = BADDR(pr->pr_CLI);
        if (cli) {
            UBYTE *bstr = BADDR(cli->cli_CommandName);
            if (bstr && bstr[0]) {
                copy_name(c->name, (const char *)bstr + 1, bstr[0]);
                return;
            }
        }
    }
    copy_name(c->name, t->tc_Node.ln_Name, CC_NAMELEN);
}

/* Bytes the transfer spans: requested up front, or prepared in chunks */
static ULONG extent(const struct Xfer *x)
{
    return x->covered > x->reqlen ? x->covered : x->reqlen;
}

static struct Xfer *alloc_xfer(void)
{
    ULONG i;

    for (i = 0; i < CC_MAX_XFERS; i++)
        if (!g_Xfers[i].used)
            return &g_Xfers[i];
    return NULL;
}

/* The transfer a DMA_Continue call at addr belongs to */
static struct Xfer *find_continuation(APTR addr, struct Task *task)
{
    struct Xfer *x, *any = NULL;
    ULONG i;

    /* Prefer an exact continuation (addr == start + covered) */
    for (i = 0; i < CC_MAX_XFERS; i++) {
        x = &g_Xfers[i];
        if (!x->used)
            continue;
        if ((UBYTE *)addr == (UBYTE *)x->start + x->covered) {
            if (x->who.task == task)
                return x;
            if (!any)
                any = x;
        }
    }
    if (any)
        return any;
    /* Otherwise anything whose requested range contains addr */
    for (i = 0; i < CC_MAX_XFERS; i++) {
        x = &g_Xfers[i];
        if (x->used && (UBYTE *)addr >= (UBYTE *)x->start &&
            (UBYTE *)addr < (UBYTE *)x->start + extent(x))
            return x;
    }
    return NULL;
}

/*
 * The transfer a CachePostDMA() at addr closes. Posts may come from a
 * different task or an interrupt, so match on address only, preferring an
 * exact start address over a containing range.
 */
static struct Xfer *find_post(APTR addr)
{
    struct Xfer *x, *inside = NULL;
    ULONG i;

    for (i = 0; i < CC_MAX_XFERS; i++) {
        x = &g_Xfers[i];
        if (!x->used)
            continue;
        if (x->start == addr)
            return x;
        if (!inside && (UBYTE *)addr > (UBYTE *)x->start &&
            (UBYTE *)addr < (UBYTE *)x->start + extent(x))
            inside = x;
    }
    return inside;
}

/* Any outstanding transfer overlapping [addr, addr + len) */
static struct Xfer *find_overlap(APTR addr, ULONG len)
{
    UBYTE *a = addr, *e = a + len;
    ULONG i;

    if (!len)
        return NULL;
    for (i = 0; i < CC_MAX_XFERS; i++) {
        struct Xfer *x = &g_Xfers[i];
        UBYTE *s = x->start;
        if (x->used && a < s + extent(x) && s < e)
            return x;
    }
    return NULL;
}

/* Record an event; call under Disable(). The daemon is signalled later. */
static void emit(UBYTE type, APTR addr, ULONG len, ULONG flags,
                 const struct Caller *who, const struct Xfer *x)
{
    struct Event *ev = &g_Events[g_EvSeq % CC_MAX_EVENTS];

    ev->type = type;
    ev->t = g_Now;
    ev->addr = addr;
    ev->len = len;
    ev->flags = flags;
    ev->who = *who;
    ev->hasXfer = x != NULL;
    if (x)
        ev->xfer = *x;
    g_EvSeq++;
    g_Stats.events[type]++;
}

static void wake_daemon(void)
{
    if (g_DaemonTask)
        Signal(g_DaemonTask, g_EvSigMask);
}

void cc_pre_hook(struct DMAFrame *f, APTR phys)
{
    struct Caller who;
    struct Xfer *x;
    ULONG granted, seq;

    (void)phys;
    capture_caller(&who, f->pc, f);
    granted = f->lenp ? *f->lenp : 0;

    Disable();
    seq = g_EvSeq;
    if ((f->flags & DMA_Continue) == 0) {
        g_Stats.starts++;
        x = find_overlap(f->addr, f->len);
        if (x)
            emit(EV_OVERLAP, f->addr, f->len, f->flags, &who, x);
        x = alloc_xfer();
        if (x) {
            x->used = 1;
            x->stale = 0;
            x->chunks = 1;
            x->start = f->addr;
            x->reqlen = f->len;
            x->covered = granted;
            x->flags = f->flags;
            x->t0 = g_Now;
            x->who = who;
            g_Stats.active++;
            if ((ULONG)g_Stats.active > g_Stats.maxActive)
                g_Stats.maxActive = (ULONG)g_Stats.active;
        } else {
            /* Report the first overflow only, not every start after it */
            if (g_Stats.untracked++ == 0)
                emit(EV_TABLE_FULL, f->addr, f->len, f->flags, &who, NULL);
        }
    } else {
        g_Stats.continues++;
        x = find_continuation(f->addr, who.task);
        if (x) {
            x->chunks++;
            if ((UBYTE *)f->addr == (UBYTE *)x->start + x->covered)
                x->covered += granted;
        } else {
            emit(EV_CONT_UNMATCHED, f->addr, f->len, f->flags, &who, NULL);
        }
    }
    seq = g_EvSeq - seq;
    Enable();

    if (seq)
        wake_daemon();
}

void cc_post_hook(struct DMAFrame *f)
{
    struct Caller who;
    struct Xfer *x;
    ULONG seq;

    capture_caller(&who, f->pc, f);

    Disable();
    seq = g_EvSeq;
    g_Stats.posts++;
    /*
     * The MuLib Programmer's Manual (7.4): CachePostDMA() is called once
     * per transfer, after it completed or was aborted, with the initial
     * logical address and the complete initial length, and with
     * DMA_ReadFromRAM as for CachePreDMA(). Only DMA_NoModify and
     * DMA_ReadFromRAM are defined for it. So every post closes the transfer
     * it matches; anything else about it is a violation.
     */
    if (f->flags & DMA_Continue)
        emit(EV_POST_CONTINUE, f->addr, f->len, f->flags, &who, NULL);
    x = find_post(f->addr);
    if (!x) {
        emit(EV_POST_UNMATCHED, f->addr, f->len, f->flags, &who, NULL);
    } else {
        if (f->addr != x->start)
            emit(EV_INSIDE, f->addr, f->len, f->flags, &who, x);
        /* The whole length: as requested, or as prepared in chunks */
        if (f->len != x->reqlen && f->len != x->covered)
            emit(EV_LENGTH, f->addr, f->len, f->flags, &who, x);
        if ((f->flags ^ x->flags) & DMA_ReadFromRAM)
            emit(EV_DIRECTION, f->addr, f->len, f->flags, &who, x);
        /* The MMU library cut the transfer short and the caller never
           continued it, yet it claims to have transferred more. */
        if (x->covered < x->reqlen && f->len > x->covered)
            emit(EV_SHORT_IGNORED, f->addr, f->len, f->flags, &who, x);
        x->used = 0;
        g_Stats.active--;
    }
    seq = g_EvSeq - seq;
    Enable();

    if (seq)
        wake_daemon();
}

/* ------------------------------------------------------------------ */
/* Patching                                                           */
/* ------------------------------------------------------------------ */

static BOOL install_patches(void)
{
    Forbid();
    g_OldPre = SetFunction((struct Library *)SysBase, LVOCachePreDMA, (APTR)cc_PreStub);
    g_OldPost = SetFunction((struct Library *)SysBase, LVOCachePostDMA, (APTR)cc_PostStub);
    Permit();
    return g_OldPre && g_OldPost;
}

/* Current target of a library vector (JMP abs.l), or NULL if it isn't one */
static APTR current_vector(LONG lvo)
{
    UBYTE *v = (UBYTE *)SysBase + lvo;
    if (*(UWORD *)v != 0x4EF9)
        return NULL;
    return *(APTR *)(v + 2);
}

/*
 * Restore the original vectors. Refuses (and changes nothing) if someone
 * else has SetFunction()ed either vector after us: restoring blindly would
 * drop their patch, and anything chained to us would jump into our freed
 * segment once we exit.
 */
static BOOL remove_patches(void)
{
    LONG waited;

    Forbid();
    if ((g_OldPre && current_vector(LVOCachePreDMA) != (APTR)cc_PreStub) ||
        (g_OldPost && current_vector(LVOCachePostDMA) != (APTR)cc_PostStub)) {
        Permit();
        return FALSE;
    }
    if (g_OldPre) {
        SetFunction((struct Library *)SysBase, LVOCachePreDMA, g_OldPre);
        g_OldPre = NULL;
    }
    if (g_OldPost) {
        SetFunction((struct Library *)SysBase, LVOCachePostDMA, g_OldPost);
        g_OldPost = NULL;
    }
    Permit();

    /*
     * A task may have been preempted inside a stub (or inside the original
     * CachePreDMA(), which returns into our stub). Wait until nobody is in
     * there before our segment can be unloaded, plus a grace period for the
     * few instructions after the in-flight counter is decremented.
     */
    for (waited = 0; g_InFlight > 0; waited++) {
        if (waited == 5 * TICKS_PER_SECOND)
            Printf((CONST_STRPTR)"CacheCop: waiting for %ld caller(s) to leave the patch...\n",
                   (LONG)g_InFlight);
        Delay(1);
    }
    Delay(TICKS_PER_SECOND / 5);
    return TRUE;
}

/*
 * Called by the daemon once a second: report transfers that have been
 * outstanding for TIMEOUT seconds, once each. A real DMA finishes in
 * milliseconds; one that never gets its CachePostDMA() stays here forever.
 */
static BOOL check_stale(void)
{
    BOOL found = FALSE;
    ULONG i;

    if (!g_Timeout)
        return FALSE;
    for (i = 0; i < CC_MAX_XFERS; i++) {
        struct Xfer *x = &g_Xfers[i];

        Disable();
        if (x->used && !x->stale && g_Now - x->t0 >= g_Timeout) {
            x->stale = 1;
            emit(EV_STALE, x->start, x->reqlen, x->flags, &x->who, x);
            found = TRUE;
        }
        Enable();
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* Naming code addresses (task context only)                          */
/* ------------------------------------------------------------------ */

static struct Library *g_ExpansionBase;

static void fmt(char *buf, const char *f, ...);

/* SegTracker (from the Enforcer package) publishes this semaphore */
struct SegSem {
    struct SignalSemaphore seg_Semaphore;
    APTR                   seg_Find;
};

const char *cc_SegFind(APTR find, APTR addr, ULONG *hunk, ULONG *offset);

#define CC_WHERELEN 48

static void copy_tail(char *dst, const char *src, ULONG size)
{
    ULONG len = strlen(src);

    /* Keep the end of long paths: the file name is what matters */
    if (len >= size)
        src += len - (size - 1);
    strcpy(dst, src);
}

/* Loaded code: ask SegTracker */
static BOOL where_segtracker(char *buf, APTR pc)
{
    struct SegSem *ss;
    const char *name = NULL;
    ULONG hunk = 0, off = 0;
    char tmp[CC_WHERELEN];

    Forbid();
    ss = (struct SegSem *)FindSemaphore((CONST_STRPTR)"SegTracker");
    if (ss && ss->seg_Find) {
        name = cc_SegFind(ss->seg_Find, pc, &hunk, &off);
        if (name)
            copy_tail(tmp, name, CC_WHERELEN - 20);
    }
    Permit();
    if (!name)
        return FALSE;
    fmt(buf, "%s:%lu+$%lx", (ULONG)tmp, hunk, off);
    return TRUE;
}

/*
 * Resident modules (Kickstart ROM and kicked modules): take the closest
 * RomTag at or below pc. rt_EndSkip normally marks the end of the module.
 * In the Kickstart ROM, where modules sit back to back, a pc past it is
 * still attributed to the preceding module, marked with '?'.
 */
static BOOL in_kickstart(APTR pc)
{
    ULONG a = (ULONG)pc;
    return (a >= 0xF80000UL && a < 0x1000000UL) ||   /* main ROM */
           (a >= 0xE00000UL && a < 0xE80000UL);      /* extended ROM */
}

static BOOL where_resident(char *buf, APTR pc, BOOL guess)
{
    ULONG *rm = (ULONG *)SysBase->ResModules;
    struct Resident *best = NULL;
    char tmp[CC_WHERELEN];

    while (rm && *rm) {
        struct Resident *r;

        if (*rm & 0x80000000UL) {        /* link to another array */
            rm = (ULONG *)(*rm & 0x7fffffffUL);
            continue;
        }
        r = (struct Resident *)*rm++;
        if ((APTR)r <= pc && (!best || r > best))
            best = r;
    }
    if (!best || !best->rt_Name)
        return FALSE;
    if (pc >= best->rt_EndSkip &&
        !(guess && in_kickstart(pc) && in_kickstart(best)))
        return FALSE;
    copy_tail(tmp, best->rt_Name, CC_WHERELEN - 16);
    fmt(buf, "%s+$%lx%s", (ULONG)tmp, (ULONG)((UBYTE *)pc - (UBYTE *)best),
        (ULONG)(pc >= best->rt_EndSkip ? "?" : ""));
    return TRUE;
}

/*
 * Libraries, devices and resources: find the jump table entry whose target
 * is closest below pc (within 64K) and report the LVO. This names drivers
 * that neither SegTracker nor the resident list know about, such as ones
 * a boot ROM relocated into RAM. A routine without its own LVO is reported
 * relative to the nearest one that precedes it.
 */
static void scan_vectors(struct List *list, APTR pc, struct Library **best,
                         ULONG *bestTarget, LONG *bestLVO)
{
    struct Node *n;

    for (n = list->lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct Library *lib = (struct Library *)n;
        UWORD i, count = lib->lib_NegSize / 6;

        for (i = 1; i <= count; i++) {
            UBYTE *v = (UBYTE *)lib - 6 * i;
            ULONG target;

            if (*(UWORD *)v != 0x4EF9)          /* JMP abs.l */
                continue;
            target = *(ULONG *)(v + 2);
            /* Our own patches would claim whatever follows our code */
            if (target == (ULONG)cc_PreStub || target == (ULONG)cc_PostStub)
                continue;
            if (target <= (ULONG)pc && (ULONG)pc - target < 0x10000UL &&
                (!*best || target > *bestTarget)) {
                *best = lib;
                *bestTarget = target;
                *bestLVO = -6 * (LONG)i;
            }
        }
    }
}

static BOOL where_library(char *buf, APTR pc)
{
    struct Library *best = NULL;
    ULONG target = 0;
    LONG lvo = 0;
    char tmp[CC_WHERELEN];

    Forbid();
    scan_vectors(&SysBase->DeviceList, pc, &best, &target, &lvo);
    scan_vectors(&SysBase->LibList, pc, &best, &target, &lvo);
    scan_vectors(&SysBase->ResourceList, pc, &best, &target, &lvo);
    if (best)
        copy_tail(tmp, best->lib_Node.ln_Name ? best->lib_Node.ln_Name : "?",
                  CC_WHERELEN - 20);
    Permit();
    if (!best)
        return FALSE;
    fmt(buf, "%s LVO%ld+$%lx", (ULONG)tmp, lvo, (ULONG)pc - target);
    return TRUE;
}

/* Code running from an expansion board's ROM (not from a RAM board) */
static BOOL where_board(char *buf, APTR pc)
{
    struct ConfigDev *cd = NULL;

    if (!g_ExpansionBase)
        return FALSE;
    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        UBYTE *base = cd->cd_BoardAddr;
        if (cd->cd_Rom.er_Type & ERTF_MEMLIST)
            continue;
        if ((UBYTE *)pc >= base && (UBYTE *)pc < base + cd->cd_BoardSize) {
            fmt(buf, "board %lu/%lu+$%lx",
                (ULONG)cd->cd_Rom.er_Manufacturer, (ULONG)cd->cd_Rom.er_Product,
                (ULONG)((UBYTE *)pc - base));
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * Describe the code at pc, or return FALSE if nothing claims it. Most
 * reliable first: SegTracker, then a resident module that contains pc,
 * then the nearest library/device entry point, an expansion ROM, and
 * finally the Kickstart module preceding pc.
 */
static BOOL where_is(char *buf, APTR pc)
{
    return where_segtracker(buf, pc) || where_resident(buf, pc, FALSE) ||
           where_library(buf, pc) || where_board(buf, pc) ||
           where_resident(buf, pc, TRUE);
}

/* ------------------------------------------------------------------ */
/* Formatting                                                         */
/* ------------------------------------------------------------------ */

extern UBYTE cc_StuffChar[];
void cc_SerPutStr(const char *s);

#define CC_LINELEN 320

/* RawDoFmt() into buf. Every argument must be 32 bits wide (use %l..). */
static void fmt(char *buf, const char *f, ...)
{
    va_list ap;

    va_start(ap, f);
    RawDoFmt((CONST_STRPTR)f, (APTR)ap, (VOID (*)())cc_StuffChar, buf);
    va_end(ap);
}

static void flags_str(char *buf, ULONG flags)
{
    buf[0] = (flags & DMA_ReadFromRAM) ? 'R' : '-';
    buf[1] = (flags & DMA_NoModify) ? 'N' : '-';
    buf[2] = (flags & DMA_Continue) ? 'C' : '-';
    buf[3] = '\0';
}

/* Name the caller's code once; call from task context only */
static void resolve(struct Caller *c)
{
    if (c->resolved)
        return;
    if (!where_is(c->where, c->pc))
        c->where[0] = '\0';
    c->resolved = 1;
}

static void fmt_caller(char *buf, const struct Caller *c)
{
    char where[CC_WHERESIZE + 4];

    where[0] = '\0';
    if (c->resolved ? c->where[0] != '\0' : where_is(where + 2, c->pc)) {
        if (c->resolved)
            strcpy(where + 2, c->where);
        where[0] = ' ';
        where[1] = '(';
        strcat(where, ")");
    }
    fmt(buf, "$%08lx%s %s%s", (ULONG)c->pc, (ULONG)where,
        (ULONG)(c->name[0] ? c->name : "?"), (ULONG)(c->inint ? " (int)" : ""));
}

static BOOL is_post_event(UBYTE type)
{
    return type == EV_POST_UNMATCHED || type == EV_DIRECTION ||
           type == EV_SHORT_IGNORED || type == EV_LENGTH || type == EV_INSIDE ||
           type == EV_POST_CONTINUE;
}

static void fmt_event(char *buf, const struct Event *ev)
{
    char fl[4], who[128];

    if (ev->type == EV_STALE) {
        /* No offending call, just the transfer */
        fmt(buf, "CacheCop [%lus] %s (%lus)\n", ev->t,
            (ULONG)kEventNames[ev->type], ev->t - ev->xfer.t0);
    } else {
        flags_str(fl, ev->flags);
        fmt_caller(who, &ev->who);
        fmt(buf, "CacheCop [%lus] %s\n  %s $%08lx len $%lx fl %s by %s\n",
            ev->t, (ULONG)kEventNames[ev->type],
            (ULONG)(is_post_event(ev->type) ? "post" : "pre "),
            (ULONG)ev->addr, ev->len, (ULONG)fl, (ULONG)who);
    }
    if (ev->hasXfer) {
        const struct Xfer *x = &ev->xfer;
        flags_str(fl, x->flags);
        fmt_caller(who, &x->who);
        fmt(buf + strlen(buf),
            "  xfer $%08lx len $%lx prepared $%lx fl %s at %lus\n"
            "       by %s\n",
            (ULONG)x->start, x->reqlen, x->covered, (ULONG)fl, x->t0, (ULONG)who);
    }
}

/* ------------------------------------------------------------------ */
/* Control messages                                                   */
/* ------------------------------------------------------------------ */

#define CC_MAGIC 0x43437034UL   /* 'CCp4': bump when the layout changes */

enum {
    CC_CMD_STATUS = 1,
    CC_CMD_RESET  = 2,
    CC_CMD_STOP   = 3,
};

/* Filled in by the daemon for CC_CMD_STATUS, in the client's memory */
struct CCStatus {
    ULONG        now;
    struct Stats stats;
    ULONG        nxfers;
    struct Xfer  xfers[CC_MAX_XFERS];
    ULONG        evseq;    /* events emitted since start */
    ULONG        nevents;  /* most recent events since RESET, oldest first */
    struct Event events[CC_MAX_EVENTS];
};

struct CCMsg {
    struct Message   msg;
    ULONG            magic;
    ULONG            cmd;
    LONG             result;   /* RETURN_OK unless the daemon says otherwise */
    struct CCStatus *status;   /* CC_CMD_STATUS */
};

/* ------------------------------------------------------------------ */
/* Daemon                                                             */
/* ------------------------------------------------------------------ */

static struct MsgPort *g_Port = NULL;
static BOOL g_StopRequested = FALSE;
static BOOL g_Serial;
static ULONG g_EvPrinted;      /* next event to print */
static ULONG g_EvResetSeq;     /* first event after the last RESET */

static void snapshot(struct CCStatus *st)
{
    ULONG i, first;

    Disable();
    st->now = g_Now;
    st->stats = g_Stats;
    st->evseq = g_EvSeq;
    Enable();

    /* One entry at a time, to keep interrupts-off windows short */
    st->nxfers = 0;
    for (i = 0; i < CC_MAX_XFERS; i++) {
        Disable();
        if (g_Xfers[i].used)
            st->xfers[st->nxfers++] = g_Xfers[i];
        Enable();
    }

    first = st->evseq > CC_MAX_EVENTS ? st->evseq - CC_MAX_EVENTS : 0;
    if (first < g_EvResetSeq)
        first = g_EvResetSeq;
    st->nevents = 0;
    for (i = first; i < st->evseq; i++) {
        Disable();
        /* Skip entries overwritten since we took evseq */
        if (g_EvSeq - i <= CC_MAX_EVENTS)
            st->events[st->nevents++] = g_Events[i % CC_MAX_EVENTS];
        Enable();
    }
}

static void reset_stats(void)
{
    ULONG i;

    /* Outstanding transfers stay: their posts are still to come */
    Disable();
    memset(&g_Stats, 0, sizeof(g_Stats));
    for (i = 0; i < CC_MAX_XFERS; i++)
        if (g_Xfers[i].used)
            g_Stats.active++;
    g_Stats.maxActive = g_Stats.active;
    g_EvResetSeq = g_EvSeq;
    Enable();
}

static void out_line(const char *buf)
{
    PutStr((CONST_STRPTR)buf);
    if (g_Serial)
        cc_SerPutStr(buf);
}

/* Name the callers of outstanding transfers that aren't named yet */
static void resolve_xfers(void)
{
    ULONG i;

    for (i = 0; i < CC_MAX_XFERS; i++) {
        struct Xfer *x = &g_Xfers[i];
        struct Caller c;
        APTR start = NULL;
        ULONG t0 = 0;
        BOOL need;

        Disable();
        need = x->used && !x->who.resolved;
        if (need) {
            c = x->who;
            start = x->start;
            t0 = x->t0;
        }
        Enable();
        if (!need)
            continue;

        resolve(&c);

        Disable();
        /* Unless the slot was reused for another transfer meanwhile */
        if (x->used && !x->who.resolved && x->start == start && x->t0 == t0 &&
            x->who.pc == c.pc) {
            strcpy(x->who.where, c.where);
            x->who.resolved = 1;
        }
        Enable();
    }
}

/* Print events emitted since the last call */
static void drain_events(void)
{
    char buf[CC_LINELEN];
    struct Event ev;
    ULONG lost, n;

    for (;;) {
        Disable();
        lost = 0;
        if (g_EvSeq - g_EvPrinted > CC_MAX_EVENTS) {
            lost = g_EvSeq - g_EvPrinted - CC_MAX_EVENTS;
            g_EvPrinted = g_EvSeq - CC_MAX_EVENTS;
        }
        if (g_EvPrinted == g_EvSeq) {
            Enable();
            break;
        }
        n = g_EvPrinted++;
        ev = g_Events[n % CC_MAX_EVENTS];
        Enable();

        /* Name the code now, while it's loaded, and keep that for EVENTS */
        resolve(&ev.who);
        if (ev.hasXfer)
            resolve(&ev.xfer.who);
        Disable();
        if (g_EvSeq - n <= CC_MAX_EVENTS) {     /* not overwritten yet */
            g_Events[n % CC_MAX_EVENTS].who = ev.who;
            g_Events[n % CC_MAX_EVENTS].xfer.who = ev.xfer.who;
        }
        Enable();

        if (lost) {
            fmt(buf, "CacheCop: %lu event(s) lost\n", lost);
            out_line(buf);
        }
        fmt_event(buf, &ev);
        out_line(buf);
    }
}

static VOID handle_cmd(struct CCMsg *m)
{
    if (m->magic != CC_MAGIC) {
        m->result = RETURN_FAIL;
        return;
    }
    switch (m->cmd) {
        case CC_CMD_STATUS:
            snapshot(m->status);
            break;
        case CC_CMD_RESET:
            reset_stats();
            break;
        case CC_CMD_STOP:
            g_StopRequested = TRUE;
            break;
        default:
            m->result = RETURN_FAIL;
            break;
    }
}

struct Timer {
    struct MsgPort     *port;
    struct timerequest *io;
    BOOL                open;
    BOOL                pending;
};

static BOOL timer_open(struct Timer *t)
{
    t->port = CreateMsgPort();
    if (!t->port)
        return FALSE;
    t->io = (struct timerequest *)CreateIORequest(t->port, sizeof(*t->io));
    if (!t->io)
        return FALSE;
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, &t->io->tr_node, 0))
        return FALSE;
    t->open = TRUE;
    return TRUE;
}

static void timer_start(struct Timer *t)
{
    t->io->tr_node.io_Command = TR_ADDREQUEST;
    t->io->tr_time.tv_secs = 1;
    t->io->tr_time.tv_micro = 0;
    SendIO(&t->io->tr_node);
    t->pending = TRUE;
}

/* TRUE if the one-second tick has fired; restarts it */
static BOOL timer_tick(struct Timer *t)
{
    if (!GetMsg(t->port))
        return FALSE;
    t->pending = FALSE;
    timer_start(t);
    return TRUE;
}

static void timer_close(struct Timer *t)
{
    if (t->pending) {
        AbortIO(&t->io->tr_node);
        WaitIO(&t->io->tr_node);
    }
    if (t->open)
        CloseDevice(&t->io->tr_node);
    if (t->io)
        DeleteIORequest(&t->io->tr_node);
    if (t->port)
        DeleteMsgPort(t->port);
}

static VOID loop_until_stop(struct Timer *timer)
{
    ULONG portsig = 1UL << g_Port->mp_SigBit;
    ULONG timersig = 1UL << timer->port->mp_SigBit;

    g_StopRequested = FALSE;
    while (!g_StopRequested) {
        ULONG sigs = Wait(portsig | timersig | g_EvSigMask | SIGBREAKF_CTRL_C);

        if (sigs & SIGBREAKF_CTRL_C)
            g_StopRequested = TRUE;
        if ((sigs & timersig) && timer_tick(timer)) {
            g_Now++;
            resolve_xfers();
            if (check_stale())
                drain_events();
        }
        if (sigs & g_EvSigMask) {
            drain_events();
            resolve_xfers();
        }
        if (sigs & portsig) {
            struct CCMsg *m;
            while ((m = (struct CCMsg *)GetMsg(g_Port)) != NULL) {
                handle_cmd(m);
                ReplyMsg(&m->msg);
            }
        }
    }
}

static LONG run_daemon(void)
{
    struct Timer timer;
    LONG rc = RETURN_FAIL;
    BYTE evsig = -1;

    memset(&timer, 0, sizeof(timer));
    if (FindPort((CONST_STRPTR)kPortName)) {
        Printf((CONST_STRPTR)"CacheCop: already running.\n");
        return RETURN_WARN;
    }
    if (!timer_open(&timer)) {
        Printf((CONST_STRPTR)"CacheCop: cannot open %s.\n", (ULONG)TIMERNAME);
        goto out;
    }
    evsig = AllocSignal(-1);
    if (evsig < 0) {
        Printf((CONST_STRPTR)"CacheCop: no free signal.\n");
        goto out;
    }
    g_EvSigMask = 1UL << evsig;
    g_DaemonTask = FindTask(NULL);

    g_Port = CreateMsgPort();
    if (!g_Port) {
        Printf((CONST_STRPTR)"CacheCop: cannot create port.\n");
        goto out;
    }
    g_Port->mp_Node.ln_Name = (char *)kPortName;
    g_Port->mp_Node.ln_Pri = 0;
    AddPort(g_Port);

    if (!install_patches()) {
        Printf((CONST_STRPTR)"CacheCop: failed to install patches.\n");
        remove_patches();
        goto out;
    }
    timer_start(&timer);

    Printf((CONST_STRPTR)"CacheCop " CC_VERSION ": started%s, timeout %lus. "
           "Ctrl-C or 'cachecop stop' to quit.\n",
           (ULONG)(g_Serial ? ", logging to serial" : ""), g_Timeout);
    if (g_Serial)
        cc_SerPutStr("CacheCop " CC_VERSION ": started\n");
    for (;;) {
        loop_until_stop(&timer);
        if (remove_patches())
            break;
        Printf((CONST_STRPTR)"CacheCop: another program has patched CachePreDMA/"
               "CachePostDMA after us; remove it first, then stop again.\n");
    }
    drain_events();
    Printf((CONST_STRPTR)"CacheCop: stopped.\n");
    rc = RETURN_OK;

out:
    g_DaemonTask = NULL;
    if (g_Port) {
        struct Message *m;
        RemPort(g_Port);
        while ((m = GetMsg(g_Port)) != NULL)
            ReplyMsg(m);
        DeleteMsgPort(g_Port);
        g_Port = NULL;
    }
    if (evsig >= 0)
        FreeSignal(evsig);
    timer_close(&timer);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Client                                                             */
/* ------------------------------------------------------------------ */

/*
 * Send a command to the running instance and wait for the reply. The port
 * lookup and PutMsg() happen under Forbid() so the daemon can't remove its
 * port in between.
 */
static LONG send_cmd(ULONG cmd, struct CCStatus *status)
{
    struct CCMsg msg;
    struct MsgPort *reply, *port;

    reply = CreateMsgPort();
    if (!reply) {
        Printf((CONST_STRPTR)"CacheCop: cannot create reply port.\n");
        return RETURN_FAIL;
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg.mn_ReplyPort = reply;
    msg.msg.mn_Length = sizeof(msg);
    msg.magic = CC_MAGIC;
    msg.cmd = cmd;
    msg.result = RETURN_OK;
    msg.status = status;

    Forbid();
    port = FindPort((CONST_STRPTR)kPortName);
    if (port)
        PutMsg(port, &msg.msg);
    Permit();

    if (port) {
        WaitPort(reply);
        (void)GetMsg(reply);
    }
    DeleteMsgPort(reply);

    if (!port) {
        Printf((CONST_STRPTR)"CacheCop: not running.\n");
        return RETURN_WARN;
    }
    if (msg.result != RETURN_OK)
        Printf((CONST_STRPTR)"CacheCop: the running instance is a different version.\n");
    return msg.result;
}

static struct CCStatus *get_status(LONG *rc)
{
    struct CCStatus *st = AllocVec(sizeof(*st), MEMF_PUBLIC | MEMF_CLEAR);

    if (!st) {
        Printf((CONST_STRPTR)"CacheCop: out of memory.\n");
        *rc = RETURN_FAIL;
        return NULL;
    }
    *rc = send_cmd(CC_CMD_STATUS, st);
    if (*rc != RETURN_OK) {
        FreeVec(st);
        return NULL;
    }
    return st;
}

static ULONG count_violations(const struct Stats *s)
{
    ULONG i, n = 0;

    for (i = 1; i < EV_COUNT; i++)
        n += s->events[i];
    return n;
}

/*
 * Outstanding transfers grouped by caller PC: a leaking driver leaves many
 * transfers from the same call site, and one line per site says it all.
 */
static void print_by_caller(const struct CCStatus *st)
{
    UBYTE done[CC_MAX_XFERS];
    char buf[CC_LINELEN], who[128];
    ULONG i, j;

    memset(done, 0, sizeof(done));
    Printf((CONST_STRPTR)"\nOutstanding transfers by caller ('cachecop status full'"
           " lists them all):\n");
    for (i = 0; i < st->nxfers; i++) {
        const struct Xfer *x = &st->xfers[i];
        ULONG count = 0, oldest = 0, stale = 0;

        if (done[i])
            continue;
        for (j = i; j < st->nxfers; j++) {
            const struct Xfer *y = &st->xfers[j];
            if (done[j] || y->who.pc != x->who.pc)
                continue;
            done[j] = 1;
            count++;
            stale += y->stale;
            if (st->now - y->t0 > oldest)
                oldest = st->now - y->t0;
        }
        fmt_caller(who, &x->who);
        fmt(buf, "  %5lu  oldest %lus, %lu timed out\n         by %s\n",
            count, oldest, stale, (ULONG)who);
        PutStr((CONST_STRPTR)buf);
    }
}

/* RETURN_WARN if any violation was seen, so scripts can test for it */
static LONG print_status(BOOL full)
{
    char buf[CC_LINELEN], who[128], fl[4];
    struct CCStatus *st;
    const struct Stats *s;
    LONG rc;
    ULONG i;

    st = get_status(&rc);
    if (!st)
        return rc;

    s = &st->stats;
    Printf((CONST_STRPTR)"CacheCop status after %lus:\n", st->now);
    Printf((CONST_STRPTR)"  outstanding: %ld (max %lu)\n", s->active, s->maxActive);
    Printf((CONST_STRPTR)"  starts: %lu  continues: %lu  posts: %lu\n",
           s->starts, s->continues, s->posts);
    if (s->untracked)
        Printf((CONST_STRPTR)"  untracked starts (table full): %lu\n", s->untracked);

    if (count_violations(s)) {
        Printf((CONST_STRPTR)"\nViolations ('cachecop events' for details):\n");
        for (i = 1; i < EV_COUNT; i++)
            if (s->events[i])
                Printf((CONST_STRPTR)"  %6lu  %s\n", s->events[i], (ULONG)kEventNames[i]);
        rc = RETURN_WARN;
    } else {
        Printf((CONST_STRPTR)"  no violations\n");
    }

    if (st->nxfers && !full)
        print_by_caller(st);
    if (st->nxfers && full) {
        Printf((CONST_STRPTR)"\nOutstanding transfers (R=from RAM, N=no modify, !=timed out):\n");
        for (i = 0; i < st->nxfers; i++) {
            const struct Xfer *x = &st->xfers[i];

            flags_str(fl, x->flags);
            fmt_caller(who, &x->who);
            fmt(buf, "  %5lus%s $%08lx len $%lx prepared $%lx chunks %lu fl %s\n"
                "          by %s\n",
                st->now - x->t0, (ULONG)(x->stale ? "!" : " "),
                (ULONG)x->start, x->reqlen, x->covered,
                (ULONG)x->chunks, (ULONG)fl, (ULONG)who);
            PutStr((CONST_STRPTR)buf);
        }
    }
    FreeVec(st);
    return rc;
}

static LONG print_events(void)
{
    char buf[CC_LINELEN];
    struct CCStatus *st;
    LONG rc;
    ULONG i;

    st = get_status(&rc);
    if (!st)
        return rc;
    for (i = 0; i < st->nevents; i++) {
        fmt_event(buf, &st->events[i]);
        PutStr((CONST_STRPTR)buf);
    }
    Printf((CONST_STRPTR)"%lu violation(s) since reset; the last %lu are kept.\n",
           count_violations(&st->stats), (ULONG)CC_MAX_EVENTS);
    if (st->nevents)
        rc = RETURN_WARN;
    FreeVec(st);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

#define TEMPLATE "COMMAND/A,ARG,SERIAL/S,TIMEOUT/K/N"
/* ARG: the address for WHERE, or FULL for STATUS */
enum { ARG_COMMAND, ARG_ARG, ARG_SERIAL, ARG_TIMEOUT, ARG_COUNT };

/* Parse $1234, 0x1234 or plain hex */
static BOOL parse_hex(const char *p, ULONG *val)
{
    ULONG v = 0;

    if (*p == '$')
        p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    if (!*p)
        return FALSE;
    for (; *p; p++) {
        UBYTE c = *p;
        if (c >= '0' && c <= '9')
            c -= '0';
        else if (c >= 'a' && c <= 'f')
            c -= 'a' - 10;
        else if (c >= 'A' && c <= 'F')
            c -= 'A' - 10;
        else
            return FALSE;
        v = (v << 4) | c;
    }
    *val = v;
    return TRUE;
}

/* WHERE: name the code at an address, e.g. a PC from a serial log */
static LONG print_where(const char *arg)
{
    char buf[CC_WHERELEN + 32];
    ULONG addr;

    if (!arg || !parse_hex(arg, &addr)) {
        Printf((CONST_STRPTR)"CacheCop: WHERE needs a hex address, like $F81234.\n");
        return RETURN_FAIL;
    }
    if (!where_is(buf, (APTR)addr)) {
        Printf((CONST_STRPTR)"$%08lx: unknown\n", addr);
        return RETURN_WARN;
    }
    Printf((CONST_STRPTR)"$%08lx: %s\n", addr, (ULONG)buf);
    return RETURN_OK;
}

static VOID usage(void)
{
    Printf((CONST_STRPTR)
        "Usage: cachecop " TEMPLATE "\n"
        "  START   install the patches and run until STOP or Ctrl-C;\n"
        "          violations are printed as they happen\n"
        "    SERIAL  also log violations to the serial port (RawPutChar)\n"
        "    TIMEOUT=n  flag transfers outstanding for n seconds (default 30, 0 = off)\n"
        "  STATUS [FULL]  show counters, violations and outstanding transfers\n"
        "          by caller; FULL lists every transfer\n"
        "  EVENTS  show the most recent violations in detail\n"
        "  RESET   clear the counters and the violation log\n"
        "  STOP    remove the patches and quit\n"
        "  WHERE <address>  name the code at an address\n"
        "STATUS and EVENTS return WARN if a violation was seen.\n");
}

int main(void)
{
    LONG args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    const char *cmd;
    LONG rc;

    g_ExpansionBase = OpenLibrary((CONST_STRPTR)"expansion.library", 37);
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"CacheCop");
        usage();
        rc = RETURN_FAIL;
        goto out;
    }
    cmd = (const char *)args[ARG_COMMAND];
    g_Serial = args[ARG_SERIAL] != 0;
    if (args[ARG_TIMEOUT])
        g_Timeout = *(ULONG *)args[ARG_TIMEOUT];

    if (!stricmp(cmd, "start"))
        rc = run_daemon();
    else if (!stricmp(cmd, "status"))
        rc = print_status(args[ARG_ARG] &&
                          !stricmp((const char *)args[ARG_ARG], "full"));
    else if (!stricmp(cmd, "events"))
        rc = print_events();
    else if (!stricmp(cmd, "reset"))
        rc = send_cmd(CC_CMD_RESET, NULL);
    else if (!stricmp(cmd, "stop"))
        rc = send_cmd(CC_CMD_STOP, NULL);
    else if (!stricmp(cmd, "where"))
        rc = print_where((const char *)args[ARG_ARG]);
    else {
        usage();
        rc = RETURN_FAIL;
    }

    FreeArgs(rda);
out:
    if (g_ExpansionBase)
        CloseLibrary(g_ExpansionBase);
    return rc;
}
