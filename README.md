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
- Runs as a small resident process with a public port `"CacheCop"` for control.

## Commands
- `cachecop start`  — install patches and run until `stop` or CTRL-C
- `cachecop status` — query counters from the running instance
- `cachecop reset`  — reset counters to zero (does not remove patches)
- `cachecop stop`   — ask the running instance to unpatch and exit

## Build
- Requires AmigaOS NDK 3.1+ (or 3.2+) includes (for LVOs and flag definitions).
- Tested build recipes (pick one and adjust paths):

  - GCC (m68k-amigaos / Bebbo):
    m68k-amigaos-gcc -O2 -noixemul -I/path/to/NDK/Include/include_h \
      -o cachecop src/cachecop.c

  - vbcc:
    vc +ami -O2 -I/path/to/NDK/Include/include_h -o cachecop src/cachecop.c

## Notes
- `DMA_Continue` (`1<<1`), `DMA_NoModify` and `DMA_ReadFromRAM` come from
  `exec/execbase.h` in any NDK 3.1+.
- By default, only `exec.library` is patched. You can optionally enable
  patching of `mmu.library` by defining `CACHECOP_PATCH_MMULIB` at compile time,
  provided you have proper headers for mmu.library (protos/LVOs) available.
- The wrappers are careful to preserve and pass through registers so original
  behaviour and return values are unchanged.

## Limitations
- This utility observes call pairing but does not verify flag mismatches (e.g.
  `DMAF_ReadFromRAM` symmetry). It aims to answer the key “did every start get
  a post?” question reliably with minimal overhead.

## License
- Public domain / Unlicense. Do whatever helps you debug your system.
