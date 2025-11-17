/*
 * CacheCop — CachePreDMA/PostDMA auditor for AmigaOS
 *
 * Increments on CachePreDMA without DMAF_Continue; decrements on every
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

#ifndef DMAF_Continue
#include <exec/io.h>
#endif

/* Fallback for NDKs without this MMULib-defined flag. */
#ifndef DMAF_Continue
#define DMAF_Continue 0x00000004UL
#endif

#ifndef LVOCachePreDMA
#include <inline/exec.h>
#endif

/* If LVO macros are absent, derive from inline/exec.h offsets:
   - CachePreDMA uses offs 0x2fa
   - CachePostDMA uses offs 0x300
   SetFunction expects negative LVO offsets. */
#if !defined(LVOCachePreDMA)
#define LVOCachePreDMA  (-0x2fa)
#endif
#if !defined(LVOCachePostDMA)
#define LVOCachePostDMA (-0x300)
#endif

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

/* Old function vectors */
static APTR g_OldCachePreDMA = NULL;
static APTR g_OldCachePostDMA = NULL;

/* Control port */
static struct MsgPort *g_Port = NULL;

/* Utility: atomic-ish inc/dec under Forbid/Permit to avoid task switching races */
static inline void cc_inc_active(void)
{
    Forbid();
    g_activeDMAs++;
    if ((ULONG)g_activeDMAs > g_maxActive)
        g_maxActive = (ULONG)g_activeDMAs;
    g_totalStarts++;
    Permit();
}

static inline void cc_dec_active(void)
{
    Forbid();
    if (g_activeDMAs > 0) {
        g_activeDMAs--;
    } else {
        g_postsWithoutStart++;
    }
    g_totalPosts++;
    Permit();
}

/* Bridge to original vectors — ensure a6=SysBase is set for libcall ABI */
static ULONG call_old_pre(APTR addr, ULONG len, ULONG flags)
{
    register APTR a0 __asm("a0") = addr;
    register ULONG d0in __asm("d0") = len;
    register ULONG d1in __asm("d1") = flags;
    register struct ExecBase *a6 __asm("a6") = SysBase;
    register ULONG d0out __asm("d0");
    __asm volatile ("jsr (%2)"
                    : "=d"(d0out)
                    : "a"(a6), "a"(g_OldCachePreDMA), "a"(a0), "d"(d0in), "d"(d1in)
                    : "cc","memory");
    return d0out;
}

static VOID call_old_post(APTR addr, ULONG len, ULONG flags)
{
    register APTR a0 __asm("a0") = addr;
    register ULONG d0in __asm("d0") = len;
    register ULONG d1in __asm("d1") = flags;
    register struct ExecBase *a6 __asm("a6") = SysBase;
    __asm volatile ("jsr (%2)"
                    :
                    : "a"(a6), "a"(g_OldCachePostDMA), "a"(a0), "d"(d0in), "d"(d1in)
                    : "cc","memory");
}

/* Replacement vectors — libcall ABI */
ULONG CacheCop_CachePreDMA(register APTR addr __asm("a0"),
                           register ULONG len __asm("d0"),
                           register ULONG flags __asm("d1"))
{
    if ((flags & DMAF_Continue) == 0) {
        cc_inc_active();
    }
    return call_old_pre(addr, len, flags);
}

VOID CacheCop_CachePostDMA(register APTR addr __asm("a0"),
                           register ULONG len __asm("d0"),
                           register ULONG flags __asm("d1"))
{
    cc_dec_active();
    call_old_post(addr, len, flags);
}

/* Patch/unpatch helpers */
static BOOL install_patches(void)
{
    if (!g_OldCachePreDMA) {
        g_OldCachePreDMA = SetFunction((struct Library *)SysBase, LVOCachePreDMA, (APTR)CacheCop_CachePreDMA);
        if (!g_OldCachePreDMA) return FALSE;
    }
    if (!g_OldCachePostDMA) {
        g_OldCachePostDMA = SetFunction((struct Library *)SysBase, LVOCachePostDMA, (APTR)CacheCop_CachePostDMA);
        if (!g_OldCachePostDMA) return FALSE;
    }
    return TRUE;
}

static VOID remove_patches(void)
{
    if (g_OldCachePreDMA) {
        SetFunction((struct Library *)SysBase, LVOCachePreDMA, g_OldCachePreDMA);
        g_OldCachePreDMA = NULL;
    }
    if (g_OldCachePostDMA) {
        SetFunction((struct Library *)SysBase, LVOCachePostDMA, g_OldCachePostDMA);
        g_OldCachePostDMA = NULL;
    }
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
            Forbid();
            g_activeDMAs = 0;
            g_maxActive = 0;
            g_totalStarts = 0;
            g_totalPosts = 0;
            g_postsWithoutStart = 0;
            Permit();
            break;
        case CC_CMD_STOP:
            /* Signal main loop to stop by setting port to NULL; main watches this */
            if (g_Port) {
                /* Tag the port userdata (mp_Flags) */
                g_Port->mp_Flags = 1;
            }
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
        if (g_Port && g_Port->mp_Flags == 1) {
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
        g_Port->mp_Flags = 0;
        AddPort(g_Port);

        Printf((CONST_STRPTR)"CacheCop: started. Ctrl-C or 'cachecop stop' to quit.\n");
        loop_until_stop();

        RemPort(g_Port);
        DeleteMsgPort(g_Port);
        g_Port = NULL;
        remove_patches();
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
