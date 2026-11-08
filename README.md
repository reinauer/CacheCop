# CacheCop — AmigaOS CachePreDMA/PostDMA auditor

## Overview
- CacheCop installs light-weight patches on `CachePreDMA()` and `CachePostDMA()`
  to count active DMA-prepared transfers. It increments a counter for every
  `CachePreDMA()` call that does NOT specify `DMA_Continue`, and decrements the
  counter for every `CachePostDMA()` call. If the counter goes negative or does
  not return to zero after activity, you likely have mismatched calls in a
  driver.

## Why
A practical way to detect misuse of the CachePreDMA/PostDMA API that can leave cache/MMU state in a bad mode.

## What it does
- Patches `exec.library` entries for `CachePreDMA` and `CachePostDMA` using
  `SetFunction()`.
- Maintains global counters:
  - active: number of outstanding DMA transfers (starts without `DMA_Continue` minus completed posts)
  - postsWithoutStart: `CachePostDMA` seen when `active <= 0`
  - maxActive: peak concurrent active value
  - totalStarts / totalPosts
- Runs in the foreground with a public port `"CacheCop"` for control. Use
  `run cachecop start` to keep the shell free.

## Commands
- `cachecop start`  — install patches and run until `stop` or CTRL-C
- `cachecop status` — query counters from the running instance
- `cachecop reset`  — reset counters to zero (does not remove patches)
- `cachecop stop`   — ask the running instance to unpatch and exit

## Build
- Requires AmigaOS NDK 3.1+ (or 3.2+) includes (for LVOs and flag definitions).
- Builds with Bebbo's m68k-amigaos-gcc. The patch stubs in `stubs.s` use GNU
  as syntax, so vbcc would need them ported to vasm.

      make NDK=/path/to/NDK/Include/include_h

  or by hand:

      m68k-amigaos-gcc -O2 -noixemul -I/path/to/NDK/Include/include_h \
        -o cachecop cachecop.c stubs.s

## Notes
- `DMA_Continue` (`1<<1`), `DMA_NoModify` and `DMA_ReadFromRAM` come from
  `exec/execbase.h` in any NDK 3.1+.
- Only `exec.library` is patched.
- The entry stubs pass a0/a1/d0/a6 through untouched, so original
  behaviour and return values are unchanged.
- `cachecop stop` refuses to remove the patches if another program has
  patched the same vectors after CacheCop; remove that program first.

## Limitations
- This utility observes call pairing but does not verify flag mismatches (e.g.
  `DMAF_ReadFromRAM` symmetry). It aims to answer the key “did every start get
  a post?” question reliably with minimal overhead.

## License
- Public domain / Unlicense. Do whatever helps you debug your system.
