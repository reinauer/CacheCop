/*
 * CacheCop — CachePreDMA/PostDMA auditor for AmigaOS
 *
 * Increments on CachePreDMA without DMA_Continue; decrements on every
 * CachePostDMA. Use STATUS/RESET/STOP via a public message port.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#ifndef Stricmp
#define Stricmp stricmp
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

/* Our ExecBase in a6 for libcall stubs */
extern struct ExecBase *SysBase;

/* Public message port name */
static const STRPTR kPortName = (STRPTR)"CacheCop";

/* State counters */
static volatile LONG g_activeDMAs = 0;
static ULONG g_maxActive = 0;
static ULONG g_totalStarts = 0;
static ULONG g_totalPosts = 0;
static ULONG g_postsWithoutStart = 0;

/* Old function vectors (referenced from stubs.s) */
APTR g_OldPre = NULL;
APTR g_OldPost = NULL;

/* Number of callers currently inside the stubs (maintained by stubs.s) */
volatile LONG g_InFlight = 0;

/* Control port */
static struct MsgPort *g_Port = NULL;
static BOOL g_StopRequested = FALSE;

/*
 * The hooks may run in interrupt context (some drivers call CachePreDMA()/
 * CachePostDMA() from their interrupt servers). Forbid() only stops task
 * switches, not interrupts, so it can't protect the counters from a hook
 * that runs in an interrupt. Disable()/Enable() can, and is legal there.
 */
static inline void cc_inc_active(void)
{
    Disable();
    g_activeDMAs++;
    if ((ULONG)g_activeDMAs > g_maxActive)
        g_maxActive = (ULONG)g_activeDMAs;
    g_totalStarts++;
    Enable();
}

static inline void cc_dec_active(void)
{
    Disable();
    if (g_activeDMAs > 0) {
        g_activeDMAs--;
    } else {
        g_postsWithoutStart++;
    }
    g_totalPosts++;
    Enable();
}

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

void cc_pre_hook(struct DMAFrame *f, APTR phys)
{
    (void)phys;
    if ((f->flags & DMA_Continue) == 0)
        cc_inc_active();
}

void cc_post_hook(struct DMAFrame *f)
{
    (void)f;
    cc_dec_active();
}

/* Patch/unpatch helpers */
static BOOL install_patches(void)
{
    if (!g_OldPre) {
        g_OldPre = SetFunction((struct Library *)SysBase, LVOCachePreDMA, (APTR)cc_PreStub);
        if (!g_OldPre) return FALSE;
    }
    if (!g_OldPost) {
        g_OldPost = SetFunction((struct Library *)SysBase, LVOCachePostDMA, (APTR)cc_PostStub);
        if (!g_OldPost) return FALSE;
    }
    return TRUE;
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

/* Control messaging */
enum {
    CC_CMD_STATUS = 1,
    CC_CMD_RESET  = 2,
    CC_CMD_STOP   = 3,
};

struct CCMsg {
    struct Message msg;
    ULONG cmd;
    /* status payload */
    LONG  active;
    ULONG maxActive;
    ULONG totalStarts;
    ULONG totalPosts;
    ULONG postsWithoutStart;
};

static VOID handle_cmd(struct CCMsg *m)
{
    switch (m->cmd) {
        case CC_CMD_STATUS:
            m->active = g_activeDMAs;
            m->maxActive = g_maxActive;
            m->totalStarts = g_totalStarts;
            m->totalPosts = g_totalPosts;
            m->postsWithoutStart = g_postsWithoutStart;
            break;
        case CC_CMD_RESET:
            Disable();
            g_activeDMAs = 0;
            g_maxActive = 0;
            g_totalStarts = 0;
            g_totalPosts = 0;
            g_postsWithoutStart = 0;
            Enable();
            break;
        case CC_CMD_STOP:
            g_StopRequested = TRUE;
            break;
    }
}

static VOID loop_until_stop(void)
{
    ULONG sigmask = (1UL << g_Port->mp_SigBit) | SIGBREAKF_CTRL_C;
    BOOL running = TRUE;
    while (running) {
        ULONG sigs = Wait(sigmask);
        if (sigs & SIGBREAKF_CTRL_C) {
            running = FALSE;
        }
        if (sigs & (1UL << g_Port->mp_SigBit)) {
            struct CCMsg *m;
            while ((m = (struct CCMsg *)GetMsg(g_Port)) != NULL) {
                handle_cmd(m);
                ReplyMsg(&m->msg);
            }
        }
        if (g_StopRequested) {
            running = FALSE;
        }
    }
}

/* Client-side helpers to send a command to a running instance */
static struct MsgPort *find_daemon(void)
{
    return FindPort(kPortName);
}

static VOID print_status(void)
{
    struct MsgPort *port = find_daemon();
    if (!port) {
        Printf((CONST_STRPTR)"CacheCop: not running.\n");
        return;
    }
    struct MsgPort *reply = CreateMsgPort();
    if (!reply) {
        Printf((CONST_STRPTR)"CacheCop: cannot create reply port.\n");
        return;
    }
    struct CCMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg.mn_ReplyPort = reply;
    msg.msg.mn_Length = sizeof(msg);
    msg.cmd = CC_CMD_STATUS;
    PutMsg(port, (struct Message *)&msg);
    WaitPort(reply);
    (void)GetMsg(reply);
    DeleteMsgPort(reply);

    /* Print values returned by daemon */
    Printf((CONST_STRPTR)"CacheCop status:\n");
    Printf((CONST_STRPTR)"  active: %ld\n", (LONG)msg.active);
    Printf((CONST_STRPTR)"  maxActive: %ld\n", (LONG)msg.maxActive);
    Printf((CONST_STRPTR)"  totalStarts: %ld\n", (LONG)msg.totalStarts);
    Printf((CONST_STRPTR)"  totalPosts: %ld\n", (LONG)msg.totalPosts);
    Printf((CONST_STRPTR)"  postsWithoutStart: %ld\n", (LONG)msg.postsWithoutStart);
}

static VOID send_cmd(ULONG cmd)
{
    struct MsgPort *port = find_daemon();
    if (!port) {
        Printf((CONST_STRPTR)"CacheCop: not running.\n");
        return;
    }
    struct MsgPort *reply = CreateMsgPort();
    if (!reply) {
        Printf((CONST_STRPTR)"CacheCop: cannot create reply port.\n");
        return;
    }
    struct CCMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg.mn_ReplyPort = reply;
    msg.msg.mn_Length = sizeof(msg);
    msg.cmd = cmd;
    PutMsg(port, (struct Message *)&msg);
    WaitPort(reply);
    (void)GetMsg(reply);
    DeleteMsgPort(reply);
}

static VOID usage(void)
{
    Printf((CONST_STRPTR)"Usage: cachecop start|status|reset|stop\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return RETURN_FAIL;
    }

    if (Stricmp(argv[1], "start") == 0) {
        if (find_daemon()) {
            Printf((CONST_STRPTR)"CacheCop: already running.\n");
            return RETURN_OK;
        }
        if (!install_patches()) {
            remove_patches();
            Printf((CONST_STRPTR)"CacheCop: failed to install patches.\n");
            return RETURN_FAIL;
        }
        g_Port = CreateMsgPort();
        if (!g_Port) {
            remove_patches();
            Printf((CONST_STRPTR)"CacheCop: cannot create port.\n");
            return RETURN_FAIL;
        }
        g_Port->mp_Node.ln_Name = (char *)kPortName;
        g_Port->mp_Node.ln_Pri = 0;
        AddPort(g_Port);

        Printf((CONST_STRPTR)"CacheCop: started. Ctrl-C or 'cachecop stop' to quit.\n");
        for (;;) {
            loop_until_stop();
            if (remove_patches())
                break;
            Printf((CONST_STRPTR)"CacheCop: another program has patched CachePreDMA/"
                   "CachePostDMA after us; remove it first, then stop again.\n");
            g_StopRequested = FALSE;
        }

        RemPort(g_Port);
        DeleteMsgPort(g_Port);
        g_Port = NULL;
        Printf((CONST_STRPTR)"CacheCop: stopped.\n");
        return RETURN_OK;
    }

    if (Stricmp(argv[1], "status") == 0) {
        print_status();
        return RETURN_OK;
    }

    if (Stricmp(argv[1], "reset") == 0) {
        send_cmd(CC_CMD_RESET);
        return RETURN_OK;
    }

    if (Stricmp(argv[1], "stop") == 0) {
        send_cmd(CC_CMD_STOP);
        return RETURN_OK;
    }

    usage();
    return RETURN_FAIL;
}
