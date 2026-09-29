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

## Results (as of the run that added this folder)

| Benchmark | gem5 cycles | RTL cycles (4L_4096V) | MAPE | Notes |
|---|---|---|---|---|
| fconv3d | 556,290 | 479,684 | 16.0% | |
| iconv2d | 193,231 | 155,748 | 24.1% | |
| fconv2d | 64,464 | 156,834 | 58.9% | large gap; embedded matrix (64x64, 7x7 filter) is plausible-sized, so this may be partially a genuine model gap rather than purely a data-size mismatch - not independently confirmed either way |
| fmatmul | 35,049 | 532,903 | 93.4% | embedded matrix is a tiny 16x64x128 (confirmed via `data.S`) - almost certainly too small to match the RTL baseline's dataset, not primarily a model-accuracy finding |
| dotproduct | 71,255 | 686 | not comparable | see below |

**dotproduct is not comparable.** Its own source has no single canonical
ROI measurement - it prints one "Vector runtime" line per inner
stripmining iteration across 4 datatypes (64b/32b/16b/8b), with no outer
timer wrap at all. We added a whole-program `[ROI-LATENCY]` wrap
(`main.c`, gated by a comment marking it as our own addition) as a
best-effort stand-in, but at 71,255 cycles vs. the RTL baseline's 686, it's
clearly measuring a much larger region than whatever narrow slice the
original RTL/paper harness used. The 686-cycle baseline almost certainly
corresponds to a single small dot-product call, not the whole multi-datatype
sweep this main() runs - we don't have the original RTL harness source to
confirm which one.

## Known gotcha (see `build.sh` for detail)

These benchmarks' `runtime.h` ties the *timer* (not just `printf`) to the
`SPIKE` macro - building with `-DSPIKE` silently makes `get_timer()` always
return 0. `build.sh` avoids this by leaving `SPIKE` undefined and instead
putting `se_build/shim` first on the include path, so `#include "printf.h"`
resolves to the shim's `#include <stdio.h>` rather than the bare-metal
embedded printf implementation.
