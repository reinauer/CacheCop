/*
 * dmatest — exercise CacheCop with correct and broken CachePreDMA()/
 * CachePostDMA() sequences.
 *
 *   dmatest ALL | OK | CHUNKS | LEAK | NOPRE | NOCONT | DIR | SHORT |
 *           LENGTH | INSIDE | POSTCONT | OVERLAP | INT | STRESS
 *
 * Each case prints what it does; compare with "cachecop status" and
 * "cachecop events". Leaked transfers point into a buffer that is never
 * freed, so CacheCop's table keeps describing real memory.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/memory.h>
#include <hardware/intbits.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

extern struct ExecBase *SysBase;

static UBYTE *buf;

static void pre(ULONG off, ULONG len, ULONG flags)
{
    ULONG l = len;
    CachePreDMA(buf + off, &l, flags);
}

static void post(ULONG off, ULONG len, ULONG flags)
{
    ULONG l = len;
    CachePostDMA(buf + off, &l, flags);
}

static void t_ok(void)
{
    Printf("OK: balanced pre/post, read and write\n");
    pre(0, 512, 0);
    post(0, 512, 0);
    pre(0, 512, DMA_ReadFromRAM);
    post(0, 512, DMA_ReadFromRAM);
}

static void t_chunks(void)
{
    Printf("CHUNKS: pre + 2 continues + one post\n");
    pre(0, 1536, 0);
    pre(512, 1024, DMA_Continue);
    pre(1024, 512, DMA_Continue);
    post(0, 1536, 0);
}

static void t_leak(void)
{
    Printf("LEAK: pre at +$1000 without post (stays outstanding)\n");
    pre(0x1000, 256, 0);
}

static void t_nopre(void)
{
    Printf("NOPRE: post at +$2000 without pre\n");
    post(0x2000, 256, 0);
}

static void t_nocont(void)
{
    Printf("NOCONT: continue at +$3000 without a first pre\n");
    pre(0x3000, 256, DMA_Continue);
}

static void t_dir(void)
{
    Printf("DIR: pre with ReadFromRAM, post without\n");
    pre(0x4000, 256, DMA_ReadFromRAM);
    post(0x4000, 256, 0);
}

static void t_short(void)
{
    /*
     * Only an MMU library that finds a physical discontinuity shortens the
     * granted length, and this test can't force one. Under MMULib with a
     * fragmented mapping, posting the full length without continuing shows
     * up as an ignored short grant; on a plain system it's a normal,
     * balanced transfer.
     */
    Printf("SHORT: (needs an MMU library that splits transfers)\n");
    pre(0x5000, 0x20000, 0);
    post(0x5000, 0x20000, 0);
}

static void t_length(void)
{
    Printf("LENGTH: post length differs from pre\n");
    pre(0x6000, 512, 0);
    post(0x6000, 256, 0);
}

static void t_inside(void)
{
    Printf("INSIDE: post address inside the transfer, not at its start\n");
    pre(0x7000, 512, 0);
    post(0x7100, 256, 0);
}

static void t_postcont(void)
{
    Printf("POSTCONT: post with DMA_Continue (closes the transfer anyway)\n");
    pre(0xB000, 512, 0);
    post(0xB000, 512, DMA_Continue);
}

static void t_overlap(void)
{
    Printf("OVERLAP: second pre over a still-outstanding transfer, then both posts\n");
    pre(0x8000, 512, 0);
    pre(0x8100, 512, 0);
    post(0x8100, 512, 0);
    post(0x8000, 512, 0);
}

/* Software interrupt that does a balanced transfer and leaks one */
static volatile ULONG softDone;

static ULONG __attribute__((used)) soft_code(void)
{
    pre(0x9000, 256, 0);
    post(0x9000, 256, 0);
    pre(0xA000, 256, 0);       /* leaked from interrupt context */
    softDone = 1;
    return 0;
}

static void t_int(void)
{
    struct Interrupt is;

    Printf("INT: balanced transfer + leak at +$A000 from a software interrupt\n");
    memset(&is, 0, sizeof(is));
    is.is_Node.ln_Type = NT_INTERRUPT;
    is.is_Node.ln_Name = "dmatest";
    is.is_Code = (void (*)(void))soft_code;
    softDone = 0;
    Cause(&is);
    while (!softDone)
        Delay(1);
}

static void t_stress(void)
{
    ULONG i;

    Printf("STRESS: 20000 balanced pairs\n");
    for (i = 0; i < 20000; i++) {
        pre((i & 15) * 256, 256, 0);
        post((i & 15) * 256, 256, 0);
    }
}

static const struct {
    const char *name;
    void (*fn)(void);
} tests[] = {
    { "OK", t_ok }, { "CHUNKS", t_chunks }, { "LEAK", t_leak },
    { "NOPRE", t_nopre }, { "NOCONT", t_nocont }, { "DIR", t_dir },
    { "SHORT", t_short }, { "LENGTH", t_length }, { "INSIDE", t_inside },
    { "POSTCONT", t_postcont },
    { "OVERLAP", t_overlap }, { "INT", t_int }, { "STRESS", t_stress },
};

int main(int argc, char **argv)
{
    ULONG i;
    BOOL all, ran = FALSE;

    if (argc < 2) {
        Printf("Usage: dmatest ALL|OK|CHUNKS|LEAK|NOPRE|NOCONT|DIR|SHORT|LENGTH|INSIDE|POSTCONT|OVERLAP|INT|STRESS\n");
        return RETURN_FAIL;
    }
    /* Never freed: leaked transfers must keep pointing at valid memory */
    buf = AllocMem(0x30000, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf)
        return RETURN_FAIL;
    Printf("dmatest: buffer at $%08lx\n", (ULONG)buf);

    /* "Run cachecop START" may not have installed its patches yet */
    for (i = 0; i < 10 * TICKS_PER_SECOND && !FindPort("CacheCop"); i++)
        Delay(1);
    if (i)
        Delay(TICKS_PER_SECOND / 2);

    all = !stricmp(argv[1], "ALL");
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (all || !stricmp(argv[1], tests[i].name)) {
            tests[i].fn();
            ran = TRUE;
        }
    }
    if (!ran)
        Printf("dmatest: unknown test %s\n", (ULONG)argv[1]);
    /* Give CacheCop a moment to print what it saw */
    Delay(TICKS_PER_SECOND);
    return ran ? RETURN_OK : RETURN_FAIL;
}
