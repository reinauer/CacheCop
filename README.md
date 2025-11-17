CacheCop — AmigaOS CachePreDMA/PostDMA auditor

Overview
- CacheCop installs light-weight patches on `CachePreDMA()` and `CachePostDMA()`
  to count active DMA-prepared transfers. It increments a counter for every
  `CachePreDMA()` call that does NOT specify `DMAF_Continue`, and decrements the
  counter for every `CachePostDMA()` call. If the counter goes negative or does
  not return to zero after activity, you likely have mismatched calls in a
  driver.

Why
A practical way to detect misuse of the CachePreDMA/PostDMA API that can leave cache/MMU state in a bad mode.

What it does
- Patches `exec.library` entries for `CachePreDMA` and `CachePostDMA` using
  `SetFunction()`.
- Maintains global counters:
  - active: number of outstanding DMA transfers (starts without `DMAF_Continue` minus completed posts)
  - postsWithoutStart: `CachePostDMA` seen when `active == 0`
  - maxActive: peak concurrent active value
  - totalStarts / totalPosts
- Runs as a small resident process with a public port `"CacheCop"` for control.

Commands
- `cachecop start`  — install patches and run until `stop` or CTRL-C
- `cachecop status` — query counters from the running instance
- `cachecop reset`  — reset counters to zero (does not remove patches)
- `cachecop stop`   — ask the running instance to unpatch and exit

Build
- Requires AmigaOS NDK 3.1+ (or 3.2+) includes (for LVOs and flag definitions).
- Tested build recipes (pick one and adjust paths):

  - GCC (m68k-amigaos / Bebbo):
    m68k-amigaos-gcc -O2 -noixemul -I/path/to/NDK/Include/include_h \
      -o cachecop src/cachecop.c

  - vbcc:
    vc +ami -O2 -I/path/to/NDK/Include/include_h -o cachecop src/cachecop.c

Notes
- The code expects `DMAF_Continue` to be defined by NDK headers (typically via
  `#include <exec/io.h>`). If you see a compile error about `DMAF_Continue`
  being undefined, ensure your include paths are set to the NDK.
- By default, only `exec.library` is patched. You can optionally enable
  patching of `mmu.library` by defining `CACHECOP_PATCH_MMULIB` at compile time,
  provided you have proper headers for mmu.library (protos/LVOs) available.
- The wrappers are careful to preserve and pass through registers so original
  behaviour and return values are unchanged.

NDK 3.2 note
- NDK 3.2 typically does not define `DMAF_Continue` out of the box (it’s part of
  MMULib documentation). CacheCop provides a guarded fallback of
  `DMAF_Continue = 0x00000004`. If you prefer to be explicit, build with:
  - `make EXTRA_CFLAGS='-DDMAF_Continue=0x4' NDK=/path/to/NDK/Include/include_h`
  - Or add the MMULib SDK headers to your include path so the flag is provided.

Limitations
- This utility observes call pairing but does not verify flag mismatches (e.g.
  `DMAF_ReadFromRAM` symmetry). It aims to answer the key “did every start get
  a post?” question reliably with minimal overhead.

License
- Public domain / Unlicense. Do whatever helps you debug your system.
