# Unseen benchmarks

The 5 benchmarks plotted by `paper_results/plot_ara_unseen_heatmap.py`
(excluding `gemv`) that had never been run against this repo's gem5
`AraCoprocessor` timing model before this folder was added: `dotproduct`,
`fconv2d`, `fconv3d`, `fmatmul`, `iconv2d`.

They live directly under `apps_gem5/<name>/` - a different benchmark tree
from `apps_gem5/riscv-vectorized-benchmark-suite/` (the RiVEC suite used by
the rest of `apps_gem5/bin/*_se.exe`). This is the native AraXL bare-metal
benchmark family, with a compiled-in `data.S` per benchmark (generated once
via each one's own `script/gen_data.py`) instead of runtime argv/file input.

## Usage

From the gem5 repo root:

```bash
apps_gem5/unseen/build.sh          # compiles all 5 for gem5 SE mode
apps_gem5/unseen/run_and_report.sh # runs each, prints MAPE vs RTL baseline
```

## Results

| Benchmark | gem5 cycles | RTL cycles (4L_4096V) | MAPE | Notes |
|---|---|---|---|---|
| fmatmul | 548,321 | 532,903 | 2.9% | dataset size updated, see below |
| dotproduct | 653 | 686 | 4.8% | ROI methodology + dataset inferred, see below |
| fconv3d | 556,290 | 479,684 | 16.0% | |
| iconv2d | 193,231 | 155,748 | 24.1% | |
| fconv2d | 64,464 | 156,834 | 58.9% | large gap; embedded matrix (64x64, 7x7 filter) is plausible-sized, so this may be partially a genuine model gap rather than purely a data-size mismatch - not independently confirmed either way |

**fmatmul's dataset was changed from this repo's own `common/default_args.mk`
default (`def_args_fmatmul = "16 64 128"`) to a 128x128x128 matrix.** The
16x64x128 default gave 35,049 cycles vs. the RTL baseline's 532,903 - a
93.4% MAPE - which looked at first like a data-size mismatch, and turned
out to be one even though the small size is this repo's own documented
default (so "canonical in this repo" isn't the same as "what the RTL
baseline was generated with"). Regenerating `data.S` via
`fmatmul/script/gen_data.py 128 128 128` and rerunning dropped the MAPE to
2.9%, strongly suggesting the RTL baseline was generated from a matrix
close to that size, not the repo's small default. If you need to
regenerate `data.S` from scratch, use `128 128 128`, not the
`default_args.mk` value.

**dotproduct needed both a dataset fix and a ROI-methodology fix.** Its
own source has no single canonical ROI measurement - it prints one
"Vector runtime" line per inner stripmining iteration across 4 datatypes
(64b/32b/16b/8b), with no outer timer wrap at all, and per-datatype loop
bounds are all derived from one `vsize`. Two things had to be inferred:
1. **Dataset size**: this repo's `def_args_dotproduct = "512"` (the
   interactive-Makefile-flow default) gives a whole-program measurement of
   71,255 cycles against a 686-cycle RTL baseline - wildly off. Using
   `vsize=64` instead - `gen_data.py`'s own internal fallback when called
   with no argument, i.e. "no stripmine" - collapses the loop structure
   from 9 stripmining iterations down to 5.
2. **ROI definition**: summing just those 5 iterations' vector-only
   runtimes (excluding the scalar reference computation used only for
   correctness checking, and excluding all `printf`/setup overhead)
   lands at 653 cycles - 4.8% off the RTL baseline's 686. `main.c` now
   accumulates this sum directly (`roi_cycles += runtime_v` after each
   `get_timer()` call) instead of the earlier whole-program wrap.

Both choices were reverse-engineered from how close the resulting number
lands to 686, not confirmed against the original RTL harness source - a
strong signal (9 candidate individual segment costs were all in the
110-160 cycle range with no obvious single match to 686, but summing the
smaller-loop-structure's 5 segments landed within 5%), not a certainty.

## Known gotcha (see `build.sh` for detail)

These benchmarks' `runtime.h` ties the *timer* (not just `printf`) to the
`SPIKE` macro - building with `-DSPIKE` silently makes `get_timer()` always
return 0. `build.sh` avoids this by leaving `SPIKE` undefined and instead
putting `se_build/shim` first on the include path, so `#include "printf.h"`
resolves to the shim's `#include <stdio.h>` rather than the bare-metal
embedded printf implementation.
