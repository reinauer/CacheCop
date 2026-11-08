# CacheCop: an AmigaOS CachePreDMA/CachePostDMA auditor

CacheCop patches exec.library's `CachePreDMA()` and `CachePostDMA()` and
reports device drivers that pair them wrongly. A driver that prepares a
buffer for DMA and never posts it can leave cache or MMU state wrong on
68040/68060 systems and with MMU libraries, and the symptom rarely points
back at the driver. CacheCop names the caller.

## What it checks

Every `CachePreDMA()` without `DMA_Continue` opens a *transfer*.
`DMA_Continue` calls extend the transfer they continue, and the
`CachePostDMA()` at the transfer's address closes it. CacheCop reports, as
they happen:

| Violation | Meaning |
|---|---|
| `CachePostDMA()` without `CachePreDMA()` | a post that matches no outstanding transfer |
| `DMA_Continue` without a first `CachePreDMA()` | a continuation of nothing |
| `CachePreDMA()` overlaps an outstanding transfer | typically a driver re-preparing a buffer it never posted |
| `DMA_ReadFromRAM` differs between pre and post | the direction flags don't match |
| posted past a shortened grant | an MMU library shortened `*length`, the caller never continued, yet posted the full length |
| outstanding longer than `TIMEOUT` | a transfer whose post never came (default 30 s) |
| table full | more than 128 transfers outstanding at once |
| `STRICT`: post length or address differs from the pre | off by default, since some drivers do this harmlessly |

Each report gives the offending call (address, length, flags) and the
transfer it relates to, each with:

- the caller's **PC**, named where possible: `name:hunk+offset` from
  SegTracker, `board mfr/prod+offset` for code in an expansion ROM, or
  `module+offset` for Kickstart and other resident modules;
- the **task**, or for Shell commands the command name;
- `(int)` if the call appears to come from an interrupt (the stack pointer
  was outside the task's stack).

## Usage

    run cachecop START [SERIAL] [STRICT] [TIMEOUT=n]
    cachecop STATUS
    cachecop EVENTS
    cachecop RESET
    cachecop STOP

- `START` installs the patches and runs until `STOP` or Ctrl-C, printing
  violations to its console. Use `run` to keep the shell free.
  - `SERIAL` also logs violations to the serial port with `RawPutChar()`,
    so they survive a crash and show up in Sashimi and similar tools.
  - `STRICT` enables the address and length checks.
  - `TIMEOUT=n` reports transfers outstanding for `n` seconds; `0` turns
    this off.
- `STATUS` shows counters, violation counts and every outstanding transfer
  with its age.
- `EVENTS` shows the last 64 violations in detail.
- `RESET` clears the counters and the violation log. Outstanding transfers
  are kept, since their posts are still to come.
- `STOP` removes the patches and quits.

`STATUS` and `EVENTS` return `WARN` when a violation was seen, so scripts
can test for it.

Example:

    CacheCop [5s] transfer outstanding longer than TIMEOUT (5s)
      xfer $0026F838 len $100 granted $100 fl --- at 0s
           by $0026CA2A DF0:dmatest

Install SegTracker (from the Enforcer package) before CacheCop to get
names for drivers loaded from disk.

## Testing

`tests/dmatest` makes correct and deliberately broken call sequences:
`dmatest ALL`, or one of `OK CHUNKS LEAK NOPRE NOCONT DIR SHORT LENGTH
INSIDE OVERLAP INT STRESS`. The release ADF boots into a self-test that
starts CacheCop with `STRICT TIMEOUT=5`, runs `dmatest ALL` and shows
`STATUS`.

## Building

Needs Bebbo's m68k-amigaos-gcc with an NDK 3.1+ (for `DMA_Continue` and
friends in `exec/execbase.h`), and `xdftool` and `lha` for the release
files.

    make            # cachecop, tests/dmatest, cachecop-<version>.lha/.adf
    make NDK=/path/to/NDK/Include/include_h
    make test-adf   # the same floppy as cachecop-test.adf

The version comes from `git describe`. Pushes and pull requests to `main`
build in CI; pushing a `v*` tag creates a draft GitHub release with the
binary, LHA and ADF.

## Notes and limitations

- The patch stubs in `stubs.s` pass `a0`/`a1`/`d0`/`a6` through untouched,
  so behaviour and return values of the original functions are unchanged.
- The checks run with interrupts disabled for a few microseconds per call.
  Output, naming and serial logging happen in the CacheCop process, never
  in the caller's context.
- Transfers started before CacheCop was installed show up as unmatched
  posts once.
- A post is matched to its transfer by address, since posts often come from
  a different task or an interrupt. A post with `DMA_Continue` at an address
  inside a transfer, but not at its start, is treated as an intermediate
  post and leaves the transfer open.
- Interrupt detection is a heuristic: `StackSwap()` users look like
  interrupts too.
- `STOP` refuses to remove the patches if another program has patched the
  same vectors after CacheCop; remove that program first. It also waits for
  callers that are still inside the patch before exiting.
- Only `exec.library` is patched.

## License

Public domain / Unlicense. Do whatever helps you debug your system.
