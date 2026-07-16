#!/usr/bin/env python3
"""
spmv_param_sweep.py — prefetcher parameter sweep driver for the RiVec spmv
benchmark, running gem5 inside the project docker image.

The swept combos mirror tsvc_results/prefetcher_params.xlsx (sheets
Stride/IMP/VIMP; per that sheet's convention the first combo of each is the
default configuration), plus a no-prefetcher base run. Two overrides are
applied on top of the spreadsheet (methodology decided 2026-07-15):

  * every prefetcher run gets prefetch_on_pf_hit=false — with the class
    default (true), hits on non-prefetched lines are never observed and
    vimp cannot train at all on spmv's partial-vl index chunks;
  * vimp additionally gets index_size=8 — spmv's ja[] is uint64_t.

Each config runs as
    docker run --rm <mounts> gem5.opt -re -d out/<sweep>/<name> ...
in a small worker pool. A DONE/FAILED marker in the run's out dir makes a
re-invocation skip completed configs, so the sweep is resumable. Every
successful run is filed into rivec_results/ via the suite's
organize_rivec_results.py (--label <config name>), and at the end the ROI
stats of all runs are collected into one workbook,
rivec_results/<input>_params.xlsx (one sheet per prefetcher, columns
matching the tsvc sheets plus vimp's internal training stats).

Usage (from the gem5 root, on the host):
    python3 rvv/spmv_param_sweep.py --smoke     # football.csr, 2 runs, ~2 min
    python3 rvv/spmv_param_sweep.py             # poisson3Db.csr, 23 runs
    python3 rvv/spmv_param_sweep.py --collect-only   # rebuild workbook only
"""

import argparse
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

GEM5_ROOT = "/home/parkw/gem5"
SUITE = "/home/parkw/riscv-vectorized-benchmark-suite"
IMAGE = "ghcr.io/gem5/ubuntu-22.04_all-dependencies:v23-0"

DOCKER = [
    "docker", "run", "--rm",
    # Run as the host user so the out tree stays writable host-side
    # (root-in-container would leave root-owned files behind).
    "--user", f"{os.getuid()}:{os.getgid()}",
    "-e", "HOME=/tmp",
    "-v", f"{GEM5_ROOT}:/gem5",
    "-v", "/home/parkw/.cache/gem5:/tmp/gem5-cache",
    "-v", "/home/parkw/ligra:/ligra",
    "-v", f"{SUITE}:/riscv-vectorized-benchmark-suite",
    "-e", "GEM5_RESOURCE_DIR=/tmp/gem5-cache",
    "-w", "/gem5",
    IMAGE,
]

# Core config fixed across the sweep (matches the interactive spmv runs).
CORE_ARGS = [
    "--vector-cache",
    "--prefetcher-side", "vector",
    "--prefetcher-level", "l1",
    "--vlen", "2048",
    "--vector-timing-throughput", "4",
    "--simd-units", "2",
]

BIN = "/riscv-vectorized-benchmark-suite/_spmv/bin/spmv_vector.exe"
INPUT_DIR = "/riscv-vectorized-benchmark-suite/_spmv/input"

# Applied to every run of the named prefetcher, on top of the swept params.
FIXED_PF_PARAMS = {
    "stride": {"prefetch_on_pf_hit": "false"},
    "imp": {"prefetch_on_pf_hit": "false"},
    "vimp": {"prefetch_on_pf_hit": "false", "index_size": "8"},
}

# Swept combos, mirroring tsvc_results/prefetcher_params.xlsx.
STRIDE_COMBOS = [(4, 0), (16, 0), (32, 0), (64, 0),
                 (4, 4), (4, 8), (4, 16), (4, 32)]
IMP_COMBOS = [(16, 4), (32, 4), (64, 4), (16, 16), (16, 32)]
VIMP_COMBOS = [
    # (indirect_delta, prefetch_threshold, stream_counter_threshold,
    #  streaming_distance, ipd_indices_per_chunk)
    (0, 2, 4, 4, 8),
    (4, 2, 4, 4, 8),
    (0, 1, 4, 4, 8),
    (0, 4, 4, 4, 8),
    (0, 2, 2, 4, 8),
    (0, 2, 4, 8, 8),
    (8, 2, 4, 8, 8),
    (0, 2, 4, 4, 4),
    (0, 2, 4, 4, 16),
]

STRIDE_KEYS = ("degree", "distance")
IMP_KEYS = ("max_prefetch_distance", "streaming_distance")
VIMP_KEYS = ("indirect_delta", "prefetch_threshold",
             "stream_counter_threshold", "streaming_distance",
             "ipd_indices_per_chunk")


def build_configs():
    """Return [(name, prefetcher, {param: value})], base run first."""
    configs = [("base", "none", {})]
    for combo in STRIDE_COMBOS:
        params = dict(zip(STRIDE_KEYS, combo))
        configs.append((f"stride_deg{combo[0]}_dist{combo[1]}",
                        "stride", params))
    for combo in IMP_COMBOS:
        params = dict(zip(IMP_KEYS, combo))
        configs.append((f"imp_mpd{combo[0]}_sd{combo[1]}", "imp", params))
    for combo in VIMP_COMBOS:
        params = dict(zip(VIMP_KEYS, combo))
        name = "vimp_id{}_pt{}_sct{}_sd{}_ipc{}".format(*combo)
        configs.append((name, "vimp", params))
    return configs


def log(logfile, msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(logfile, "a") as f:
        f.write(line + "\n")


def run_one(cfg, input_name, outroot, logfile, timeout_s):
    """Run one config in docker; returns (name, 'done'|'skipped'|'failed')."""
    name, prefetcher, params = cfg
    outdir = os.path.join(outroot, name)
    done = os.path.join(GEM5_ROOT, outdir, "DONE")
    failed = os.path.join(GEM5_ROOT, outdir, "FAILED")
    if os.path.exists(done):
        log(logfile, f"{name}: already done, skipping")
        return name, "skipped"
    if os.path.exists(failed):
        os.remove(failed)  # retry previously failed runs
    # Pre-create the out dir host-side so its ownership is the host user's.
    os.makedirs(os.path.join(GEM5_ROOT, outdir), exist_ok=True)

    pf_args = []
    if prefetcher != "none":
        for k, v in {**FIXED_PF_PARAMS[prefetcher], **params}.items():
            pf_args += ["--pf-param", f"{k}={v}"]

    cmd = DOCKER + [
        "build/RISCV/gem5.opt", "-re", "-d", outdir,
        "rvv/riscv-rvv-se-ara-prefetcher.py",
        "--prefetcher", prefetcher,
    ] + pf_args + CORE_ARGS + [
        "-p", f"{INPUT_DIR}/{input_name}.csr {INPUT_DIR}/{input_name}.verif",
        BIN,
    ]

    log(logfile, f"{name}: starting")
    t0 = time.time()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout_s)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        rc = -1
        log(logfile, f"{name}: TIMEOUT after {timeout_s}s")

    host_outdir = os.path.join(GEM5_ROOT, outdir)
    simout = os.path.join(host_outdir, "simout.txt")
    verified = (os.path.exists(simout)
                and "Verification pass" in open(simout).read())
    stats = os.path.join(host_outdir, "stats.txt")
    have_stats = os.path.exists(stats) and os.path.getsize(stats) > 0

    mins = (time.time() - t0) / 60
    if rc == 0 and verified and have_stats:
        # File the run into the rivec_results record tree.
        org = subprocess.run(
            ["python3", os.path.join(SUITE, "organize_rivec_results.py"),
             "--size", input_name, "--label", name, host_outdir],
            capture_output=True, text=True)
        filed = "filed" if org.returncode == 0 else \
            f"organizer FAILED: {org.stderr.strip()[:200]}"
        open(os.path.join(host_outdir, "DONE"), "w").write("ok\n")
        log(logfile, f"{name}: done in {mins:.1f} min, verified, {filed}")
        return name, "done"

    with open(os.path.join(host_outdir, "FAILED"), "w") as fh:
        fh.write(f"rc={rc} verified={verified} have_stats={have_stats}\n")
    log(logfile, f"{name}: FAILED (rc={rc} verified={verified} "
                 f"stats={have_stats}) after {mins:.1f} min")
    return name, "failed"


# ---------------------------------------------------------------- collection

STAT_PATTERNS = {
    "Instructions": r"^simInsts\s+(\S+)",
    "Cycles": r"^board\.processor\.cores\.core\.numCycles\s+(\S+)",
    "WallClock": r"^hostSeconds\s+(\S+)",
    "VL1Accesses":
        r"^board\.cache_hierarchy\.vector-l1d-cache-0\."
        r"overallAccesses::total\s+(\S+)",
    "VL1Misses":
        r"^board\.cache_hierarchy\.vector-l1d-cache-0\."
        r"overallMisses::total\s+(\S+)",
    "Issued": r"\.prefetcher\.pfIssued\s+(\S+)",
    "Useful": r"\.prefetcher\.pfUseful\s+(\S+)",
    "DemandMshrMisses": r"\.prefetcher\.demandMshrMisses\s+(\S+)",
    "streamCandidates": r"\.prefetcher\.streamCandidates\s+(\S+)",
    "indirectCandidates": r"\.prefetcher\.indirectCandidates\s+(\S+)",
    "patternsDetected": r"\.prefetcher\.patternsDetected\s+(\S+)",
    "confidenceMatches": r"\.prefetcher\.confidenceMatches\s+(\S+)",
    "chunksSliced": r"\.prefetcher\.chunksSliced\s+(\S+)",
}


def parse_roi_stats(stats_path):
    """Parse the first (ROI) stats section into {key: float}."""
    text = open(stats_path).read()
    sections = text.split("---------- Begin Simulation Statistics ----------")
    roi = sections[1] if len(sections) > 1 else ""
    out = {}
    for key, pat in STAT_PATTERNS.items():
        m = re.search(pat, roi, re.MULTILINE)
        if m:
            try:
                out[key] = float(m.group(1))
            except ValueError:
                pass
    return out


def metric_row(s):
    insts, cycles = s.get("Instructions"), s.get("Cycles")
    acc, miss = s.get("VL1Accesses"), s.get("VL1Misses")
    issued, useful = s.get("Issued"), s.get("Useful")
    dmm = s.get("DemandMshrMisses")
    ipc = insts / cycles if insts and cycles else None
    miss_rate = miss / acc if acc else None
    accuracy = useful / issued if issued else None
    coverage = useful / (useful + dmm) if useful is not None and \
        dmm is not None and (useful + dmm) > 0 else None
    return [insts, cycles, ipc, miss_rate, issued, accuracy, coverage,
            s.get("WallClock")]


METRIC_HDR = ["Instructions", "Cycles", "IPC", "VL1 Miss Rate", "Issued",
              "Accuracy", "Coverage", "ROI Host Seconds"]
VIMP_EXTRA = ["streamCandidates", "indirectCandidates", "patternsDetected",
              "confidenceMatches", "chunksSliced"]


def collect(configs, outroot, workbook, input_name):
    import openpyxl
    wb = openpyxl.Workbook()
    wb.remove(wb.active)
    sheets = {
        "Base": ["Config"] + METRIC_HDR + ["Status"],
        "Stride": ["Config"] + list(STRIDE_KEYS) + METRIC_HDR + ["Status"],
        "IMP": ["Config"] + list(IMP_KEYS) + METRIC_HDR + ["Status"],
        "VIMP": ["Config"] + list(VIMP_KEYS) + METRIC_HDR + VIMP_EXTRA
                + ["Status"],
    }
    for title, hdr in sheets.items():
        wb.create_sheet(title).append(hdr)

    readme = wb.create_sheet("README", 0)
    for line in [
        f"spmv ({input_name}) prefetcher parameter sweep",
        "Input: RiVec spmv_vector.exe, binary CSR image; "
        "vlen=2048, lanes=4, simd-units=2, vector-cache, "
        "prefetcher on vector L1D.",
        "All prefetchers: prefetch_on_pf_hit=false; vimp: index_size=8.",
        "Stats are the kernel ROI section (m5 markers); Instructions = "
        "simInsts, Cycles = core numCycles, VL1 = vector-l1d-cache-0.",
        "Raw runs: out tree per config; record tree: "
        "rivec_results/<prefetcher>/spmv/vector/" + input_name + "/<config>/",
    ]:
        readme.append([line])

    for name, prefetcher, params in configs:
        host_outdir = os.path.join(GEM5_ROOT, outroot, name)
        stats_path = os.path.join(host_outdir, "stats.txt")
        status = "done" if os.path.exists(
            os.path.join(host_outdir, "DONE")) else "FAILED/missing"
        s = parse_roi_stats(stats_path) if os.path.exists(stats_path) else {}
        row = metric_row(s)
        if prefetcher == "none":
            wb["Base"].append([name] + row + [status])
        elif prefetcher == "stride":
            wb["Stride"].append(
                [name] + [params[k] for k in STRIDE_KEYS] + row + [status])
        elif prefetcher == "imp":
            wb["IMP"].append(
                [name] + [params[k] for k in IMP_KEYS] + row + [status])
        elif prefetcher == "vimp":
            wb["VIMP"].append(
                [name] + [params[k] for k in VIMP_KEYS] + row
                + [s.get(k) for k in VIMP_EXTRA] + [status])

    os.makedirs(os.path.dirname(workbook), exist_ok=True)
    wb.save(workbook)
    return workbook


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", action="store_true",
                    help="football.csr, base + default vimp only")
    ap.add_argument("--collect-only", action="store_true",
                    help="skip runs, just (re)build the workbook")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--input", default="poisson3Db",
                    help="input basename under _spmv/input (csr+verif)")
    ap.add_argument("--timeout-mins", type=int, default=240,
                    help="per-run timeout")
    args = ap.parse_args()

    input_name = "football" if args.smoke else args.input
    configs = build_configs()
    if args.smoke:
        configs = [c for c in configs
                   if c[0] in ("base", "vimp_id0_pt2_sct4_sd4_ipc8")]

    outroot = f"out/sweep_{input_name}"
    workbook = os.path.join(GEM5_ROOT, "rivec_results",
                            f"{input_name}_params.xlsx")
    host_outroot = os.path.join(GEM5_ROOT, outroot)
    os.makedirs(host_outroot, exist_ok=True)
    logfile = os.path.join(host_outroot, "sweep.log")

    if not args.collect_only:
        log(logfile, f"sweep start: {len(configs)} configs on {input_name}, "
                     f"{args.jobs} parallel")
        results = {}
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futs = [pool.submit(run_one, c, input_name, outroot, logfile,
                                args.timeout_mins * 60) for c in configs]
            for f in futs:
                name, status = f.result()
                results[name] = status
        n_fail = sum(1 for v in results.values() if v == "failed")
        log(logfile, f"sweep finished: {len(results) - n_fail} ok, "
                     f"{n_fail} failed")

    wb = collect(configs, outroot, workbook, input_name)
    log(logfile, f"workbook written: {wb}")
    n_failed = sum(1 for c in configs if not os.path.exists(
        os.path.join(GEM5_ROOT, outroot, c[0], "DONE")))
    sys.exit(1 if n_failed else 0)


if __name__ == "__main__":
    main()
