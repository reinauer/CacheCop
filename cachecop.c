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
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#define CC_VERSION "1.0"

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
    "$VER: CacheCop " CC_VERSION " (08.11.2026)";

/* ------------------------------------------------------------------ */
/* Shared state (written by the hooks, any context)                   */
/* ------------------------------------------------------------------ */

#define CC_MAX_XFERS 128
#define CC_NAMELEN   24

/* Where a call came from */
struct Caller {
    APTR         pc;       /* return address of the CachePre/PostDMA() call */
    struct Task *task;     /* SysBase->ThisTask at the time */
    UBYTE        inint;    /* stack pointer was outside the task's stack */
    UBYTE        pad;
    char         name[CC_NAMELEN];  /* task or CLI command name */
};

/* One DMA transfer, from its first CachePreDMA() to its CachePostDMA() */
struct Xfer {
    UBYTE  used;
    UBYTE  pad;
    UWORD  chunks;         /* CachePreDMA() calls, including the first */
    APTR   start;          /* vaddress of the first call */
    ULONG  reqlen;         /* length requested by the first call */
    ULONG  covered;        /* bytes granted so far, contiguous from start */
    ULONG  flags;          /* flags of the first call */
    ULONG  t0;             /* g_Now when the transfer started */
    struct Caller who;     /* caller of the first call */
};

struct Stats {
    LONG  active;          /* transfers currently in the table */
    ULONG maxActive;
    ULONG starts;          /* CachePreDMA() without DMA_Continue */
    ULONG continues;       /* CachePreDMA() with DMA_Continue */
    ULONG posts;           /* CachePostDMA() */
    ULONG unmatchedPosts;  /* CachePostDMA() matching no transfer */
    ULONG unmatchedConts;  /* DMA_Continue matching no transfer */
    ULONG untracked;       /* starts dropped because the table was full */
};

static struct Xfer  g_Xfers[CC_MAX_XFERS];
static struct Stats g_Stats;

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
            (UBYTE *)addr < (UBYTE *)x->start + x->reqlen)
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
            (UBYTE *)addr < (UBYTE *)x->start + x->reqlen)
            inside = x;
    }
    return inside;
}

void cc_pre_hook(struct DMAFrame *f, APTR phys)
{
    struct Caller who;
    struct Xfer *x;
    ULONG granted;

    (void)phys;
    capture_caller(&who, f->pc, f);
    granted = f->lenp ? *f->lenp : 0;

    Disable();
    if ((f->flags & DMA_Continue) == 0) {
        g_Stats.starts++;
        x = alloc_xfer();
        if (x) {
            x->used = 1;
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
            g_Stats.untracked++;
        }
    } else {
        g_Stats.continues++;
        x = find_continuation(f->addr, who.task);
        if (x) {
            x->chunks++;
            if ((UBYTE *)f->addr == (UBYTE *)x->start + x->covered)
                x->covered += granted;
        } else {
            g_Stats.unmatchedConts++;
        }
    }
    Enable();
}

void cc_post_hook(struct DMAFrame *f)
{
    struct Xfer *x;

    Disable();
    g_Stats.posts++;
    x = find_post(f->addr);
    if (!x) {
        g_Stats.unmatchedPosts++;
    } else if ((f->flags & DMA_Continue) && f->addr != x->start) {
        /* Intermediate post for a later chunk; the transfer stays open */
    } else {
        x->used = 0;
        g_Stats.active--;
    }
    Enable();
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

/* ------------------------------------------------------------------ */
/* Control messages                                                   */
/* ------------------------------------------------------------------ */

#define CC_MAGIC 0x43437031UL   /* 'CCp1': bump when the layout changes */

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

static void snapshot(struct CCStatus *st)
{
    ULONG i;

    Disable();
    st->now = g_Now;
    st->stats = g_Stats;
    Enable();

    /* One entry at a time, to keep interrupts-off windows short */
    st->nxfers = 0;
    for (i = 0; i < CC_MAX_XFERS; i++) {
        Disable();
        if (g_Xfers[i].used)
            st->xfers[st->nxfers++] = g_Xfers[i];
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
    Enable();
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
        ULONG sigs = Wait(portsig | timersig | SIGBREAKF_CTRL_C);

        if (sigs & SIGBREAKF_CTRL_C)
            g_StopRequested = TRUE;
        if ((sigs & timersig) && timer_tick(timer))
            g_Now++;
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

    memset(&timer, 0, sizeof(timer));
    if (FindPort((CONST_STRPTR)kPortName)) {
        Printf((CONST_STRPTR)"CacheCop: already running.\n");
        return RETURN_WARN;
    }
    if (!timer_open(&timer)) {
        Printf((CONST_STRPTR)"CacheCop: cannot open %s.\n", (ULONG)TIMERNAME);
        goto out;
    }
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

    Printf((CONST_STRPTR)"CacheCop " CC_VERSION ": started. Ctrl-C or 'cachecop stop' to quit.\n");
    for (;;) {
        loop_until_stop(&timer);
        if (remove_patches())
            break;
        Printf((CONST_STRPTR)"CacheCop: another program has patched CachePreDMA/"
               "CachePostDMA after us; remove it first, then stop again.\n");
    }
    Printf((CONST_STRPTR)"CacheCop: stopped.\n");
    rc = RETURN_OK;

out:
    if (g_Port) {
        struct Message *m;
        RemPort(g_Port);
        while ((m = GetMsg(g_Port)) != NULL)
            ReplyMsg(m);
        DeleteMsgPort(g_Port);
        g_Port = NULL;
    }
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

static void flags_str(char *buf, ULONG flags)
{
    buf[0] = (flags & DMA_ReadFromRAM) ? 'R' : '-';
    buf[1] = (flags & DMA_NoModify) ? 'N' : '-';
    buf[2] = '\0';
}

static void print_caller(const struct Caller *c)
{
    Printf((CONST_STRPTR)"$%08lx  %s%s", (ULONG)c->pc,
           (ULONG)(c->name[0] ? c->name : "?"), (ULONG)(c->inint ? " (int)" : ""));
}

static LONG print_status(void)
{
    struct CCStatus *st;
    const struct Stats *s;
    LONG rc;
    ULONG i;

    st = AllocVec(sizeof(*st), MEMF_PUBLIC | MEMF_CLEAR);
    if (!st) {
        Printf((CONST_STRPTR)"CacheCop: out of memory.\n");
        return RETURN_FAIL;
    }
    rc = send_cmd(CC_CMD_STATUS, st);
    if (rc != RETURN_OK)
        goto out;

    s = &st->stats;
    Printf((CONST_STRPTR)"CacheCop status after %lus:\n", st->now);
    Printf((CONST_STRPTR)"  outstanding: %ld (max %lu)\n", s->active, s->maxActive);
    Printf((CONST_STRPTR)"  starts: %lu  continues: %lu  posts: %lu\n",
           s->starts, s->continues, s->posts);
    Printf((CONST_STRPTR)"  unmatched posts: %lu  unmatched continues: %lu\n",
           s->unmatchedPosts, s->unmatchedConts);
    if (s->untracked)
        Printf((CONST_STRPTR)"  untracked starts (table full): %lu\n", s->untracked);

    if (st->nxfers) {
        Printf((CONST_STRPTR)"\nOutstanding transfers (R = from RAM, N = no modify):\n");
        Printf((CONST_STRPTR)"     age  address    requested  granted    chunks fl  caller\n");
        for (i = 0; i < st->nxfers; i++) {
            const struct Xfer *x = &st->xfers[i];
            char fl[3];

            flags_str(fl, x->flags);
            Printf((CONST_STRPTR)"  %5lus  $%08lx  $%08lx  $%08lx  %5lu  %s  ",
                   st->now - x->t0, (ULONG)x->start, x->reqlen, x->covered,
                   (ULONG)x->chunks, (ULONG)fl);
            print_caller(&x->who);
            Printf((CONST_STRPTR)"\n");
        }
    }
out:
    FreeVec(st);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

#define TEMPLATE "COMMAND/A"
enum { ARG_COMMAND, ARG_COUNT };

static VOID usage(void)
{
    Printf((CONST_STRPTR)
        "Usage: cachecop START|STATUS|RESET|STOP\n"
        "  START   install the patches and run until STOP or Ctrl-C\n"
        "  STATUS  show counters and outstanding transfers\n"
        "  RESET   clear the counters\n"
        "  STOP    remove the patches and quit\n");
}

int main(void)
{
    LONG args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    const char *cmd;
    LONG rc;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"CacheCop");
        usage();
        return RETURN_FAIL;
    }
    cmd = (const char *)args[ARG_COMMAND];

    if (!stricmp(cmd, "start"))
        rc = run_daemon();
    else if (!stricmp(cmd, "status"))
        rc = print_status();
    else if (!stricmp(cmd, "reset"))
        rc = send_cmd(CC_CMD_RESET, NULL);
    else if (!stricmp(cmd, "stop"))
        rc = send_cmd(CC_CMD_STOP, NULL);
    else {
        usage();
        rc = RETURN_FAIL;
    }

    FreeArgs(rda);
    return rc;
}
