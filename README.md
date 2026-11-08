# CacheCop: an AmigaOS CachePreDMA/CachePostDMA auditor

CacheCop patches exec.library's `CachePreDMA()` and `CachePostDMA()` and
reports device drivers that pair them wrongly. A driver that prepares a
buffer for DMA and never posts it can leave cache or MMU state wrong on
68040/68060 systems and with MMU libraries, and the symptom rarely points
back at the driver. CacheCop names the caller.

## What it checks

Every `CachePreDMA()` without `DMA_Continue` opens a *transfer*.
`DMA_Continue` calls extend the transfer they continue, and the
`CachePostDMA()` at the transfer's address closes it. The rules are those
of *The MuLib Programmer's Manual*, section 7.4: `CachePostDMA()` is called
exactly once per transfer, after it completed or was aborted, with the
initial address, the complete initial length and the same
`DMA_ReadFromRAM` as the pre; only `DMA_NoModify` and `DMA_ReadFromRAM` are
defined for it. CacheCop reports, as they happen:

| Violation | Meaning |
|---|---|
| `CachePostDMA()` without `CachePreDMA()` | a post that matches no outstanding transfer |
| `DMA_Continue` without a first `CachePreDMA()` | a continuation of nothing |
| `CachePreDMA()` overlaps an outstanding transfer | typically a driver re-preparing a buffer it never posted |
| `DMA_ReadFromRAM` differs between pre and post | the direction flags don't match |
| post address is not the transfer's initial address | e.g. a post with the address of the buffer *pointer* |
| post length is not the transfer's initial length | e.g. a post with a length `CachePreDMA()` shortened |
| `DMA_Continue` passed to `CachePostDMA()` | the post still closes the transfer |
| posted past a shortened grant | an MMU library shortened `*length` and the caller never continued (or aborted the transfer) |
| outstanding longer than `TIMEOUT` | a transfer whose post never came (default 30 s) |
| table full | more than 128 transfers outstanding at once |

Each report gives the offending call (address, length, flags) and the
transfer it relates to, each with:

- the caller's **PC**, named where possible, most reliable first:
  `name:hunk+offset` from SegTracker; `module+offset` for a resident
  module that contains it; `name LVO-n+offset` for code following the
  nearest entry point of a library, device or resource (this names drivers
  a boot ROM relocated into RAM, which SegTracker doesn't know); `board
  mfr/prod+offset` for code running in an expansion ROM; and for the
  Kickstart ROM, the preceding module, marked `?`;
- the **task**, or for Shell commands the command name;
- `(int)` if the call appears to come from an interrupt (the stack pointer
  was outside the task's stack).

## Usage

    run cachecop START [SERIAL] [TIMEOUT=n]
    cachecop STATUS
    cachecop EVENTS
    cachecop RESET
    cachecop STOP
    cachecop WHERE <address>

- `START` installs the patches and runs until `STOP` or Ctrl-C, printing
  violations to its console. Use `run` to keep the shell free.
  - `SERIAL` also logs violations to the serial port with `RawPutChar()`,
    so they survive a crash and show up in Sashimi and similar tools.
  - `TIMEOUT=n` reports transfers outstanding for `n` seconds; `0` turns
    this off.
- `STATUS` shows counters, violation counts and every outstanding transfer
  with its age.
- `EVENTS` shows the last 64 violations in detail.
- `RESET` clears the counters and the violation log. Outstanding transfers
  are kept, since their posts are still to come.
- `STOP` removes the patches and quits.
- `WHERE <address>` names the code at an address (`$F81234`, `0xF81234`
  or `F81234`), for example a PC from a serial log. It doesn't need
  CacheCop to be running.

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
INSIDE POSTCONT OVERLAP INT STRESS`; add `WAIT` to wait for a CacheCop
started with `run`. `dmatest DISK <device> [unit]` reads (never writes)
through a real driver at every buffer alignment, so its calls can be
checked.

The release ADF boots into a self-test that starts CacheCop with
`TIMEOUT=5`, runs `dmatest ALL WAIT` and shows `STATUS`.

`make test-adf` builds `cachecop-test.adf` for emulator runs: the same
self-test, plus SegTracker (downloaded from Aminet's Enforcer archive,
whose license doesn't allow it on the release floppy) and `dmatest DISK`
on `TEST_DEVICE`/`TEST_UNIT` (default `a4091.device` unit 0).

## Building

Needs Bebbo's m68k-amigaos-gcc with an NDK 3.1+ (for `DMA_Continue` and
friends in `exec/execbase.h`), and `xdftool` and `lha` for the release
files.

    make            # cachecop, tests/dmatest, cachecop-<version>.lha/.adf
    make NDK=/path/to/NDK/Include/include_h
    make test-adf   # the same floppy as cachecop-test.adf

The version comes from `git describe`. Pushes and pull requests to `main`
build in CI; pushing a `v*` tag creates a draft GitHub release with the
binary, LHA and ADF, and publishing that release uploads the LHA and
readme to Aminet (dev/debug).

## Notes and limitations

- The patch stubs in `stubs.s` pass `a0`/`a1`/`d0`/`a6` through untouched,
  so behaviour and return values of the original functions are unchanged.
- The checks run with interrupts disabled for a few microseconds per call.
  Output, naming and serial logging happen in the CacheCop process, never
  in the caller's context.
- Transfers started before CacheCop was installed show up as unmatched
  posts once.
- A post is matched to its transfer by address, since posts often come from
  a different task or an interrupt: the transfer starting there, or else
  the one containing the address.
- Interrupt detection is a heuristic: `StackSwap()` users look like
  interrupts too.
- `STOP` refuses to remove the patches if another program has patched the
  same vectors after CacheCop; remove that program first. It also waits for
  callers that are still inside the patch before exiting.
- Only `exec.library` is patched.

## License

Public domain / Unlicense. Do whatever helps you debug your system.
