/*
 * dmatest — exercise CacheCop with correct and broken CachePreDMA()/
 * CachePostDMA() sequences.
 *
 *   dmatest <test> [WAIT]
 *       test: ALL | OK | CHUNKS | LEAK | NOPRE | NOCONT | DIR | SHORT |
 *             LENGTH | INSIDE | POSTCONT | OVERLAP | INT | STRESS
 *       WAIT: wait up to 10s for CacheCop to start (for "Run cachecop")
 *
 *   dmatest DISK <device> [unit]
 *       read from a real block device (never writes), at every buffer
 *       alignment, so its driver's CachePreDMA()/CachePostDMA() calls
 *       can be checked
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
#include <exec/io.h>
#include <devices/trackdisk.h>
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

/* Read from a real device at all four buffer alignments */
static int disk_test(const char *devname, ULONG unit)
{
    struct MsgPort *port;
    struct IOStdReq *io;
    UBYTE *dbuf;
    ULONG i, ok = 0, failed = 0;

    Printf("DISK: reading from %s unit %lu\n", (ULONG)devname, unit);
    port = CreateMsgPort();
    io = port ? (struct IOStdReq *)CreateIORequest(port, sizeof(*io)) : NULL;
    dbuf = AllocMem(65536 + 16, MEMF_PUBLIC);
    if (!io || !dbuf) {
        Printf("DISK: out of memory\n");
        goto out;
    }
    if (OpenDevice((CONST_STRPTR)devname, unit, (struct IORequest *)io, 0)) {
        Printf("DISK: can't open %s unit %lu (error %ld)\n",
               (ULONG)devname, unit, (LONG)io->io_Error);
        goto out;
    }
    for (i = 0; i < 64; i++) {
        io->io_Command = CMD_READ;
        io->io_Data = dbuf + (i & 3);            /* 0..3 byte misalignment */
        io->io_Length = 512 * (1 + (i % 32));    /* 512 bytes .. 16K */
        io->io_Offset = 512 * i;
        if (DoIO((struct IORequest *)io) == 0)
            ok++;
        else
            failed++;
    }
    Printf("DISK: %lu reads ok, %lu failed\n", ok, failed);
    CloseDevice((struct IORequest *)io);
out:
    if (dbuf)
        FreeMem(dbuf, 65536 + 16);
    if (io)
        DeleteIORequest((struct IORequest *)io);
    if (port)
        DeleteMsgPort(port);
    return failed ? RETURN_WARN : RETURN_OK;
}

static ULONG parse_ulong(const char *p)
{
    ULONG v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    return v;
}

int main(int argc, char **argv)
{
    ULONG i;
    BOOL all, ran = FALSE;
    int rc;

    if (argc < 2) {
        Printf("Usage: dmatest <test> [WAIT] | DISK <device> [unit]\n"
               "tests: ALL OK CHUNKS LEAK NOPRE NOCONT DIR SHORT LENGTH INSIDE\n"
               "       POSTCONT OVERLAP INT STRESS\n");
        return RETURN_FAIL;
    }
    if (!stricmp(argv[1], "DISK")) {
        if (argc < 3) {
            Printf("Usage: dmatest DISK <device> [unit]\n");
            return RETURN_FAIL;
        }
        rc = disk_test(argv[2], argc > 3 ? parse_ulong(argv[3]) : 0);
        Delay(TICKS_PER_SECOND);
        return rc;
    }

    /* Never freed: leaked transfers must keep pointing at valid memory */
    buf = AllocMem(0x30000, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf)
        return RETURN_FAIL;
    Printf("dmatest: buffer at $%08lx\n", (ULONG)buf);

    /* "Run cachecop START" may not have installed its patches yet */
    if (argc > 2 && !stricmp(argv[2], "WAIT")) {
        for (i = 0; i < 10 * TICKS_PER_SECOND && !FindPort("CacheCop"); i++)
            Delay(1);
        Delay(TICKS_PER_SECOND / 2);
    }

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
