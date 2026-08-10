#!/usr/bin/env python3
"""
organize_results.py — file a gem5 TSVC run's output into the tsvc_results tree.

Layout produced (one directory per kernel that ran):

    tsvc_results/<size>/<kernel>/<prefetcher>/iter_<N>/<timestamp>/
        stats.txt      <- just this kernel's ROI section, split out of the run
        config.ini, config.json, citations.bib, ...   <- the run's config files (copied)
        run_info.txt   <- size, kernel, prefetcher, iterations, LEN_1D/2D, source dir

The <timestamp> level (current date-time, or --label) keeps each run of a given
size/kernel/prefetcher/iter in its own directory instead of overwriting, so you can
compare different configurations side by side.

You provide:
    --size {tiny,small,medium}   the array-size label (set at compile time; you name it)
    OUTDIR                       a gem5 -d output directory (e.g. out/stride)

Everything else is auto-detected from the run, because array size and iterations are
baked in at compile time and the prefetcher/kernels are recorded by gem5:
    iterations, LEN_1D, LEN_2D <- tsvc-gem5/src/common.h (first uncommented #define)
    prefetcher label           <- OUTDIR/config.ini [*.prefetcher] section type, short-named
                                  via the PREFETCHERS registry in rvv/prefetcher_factory.py
                                  (so new --prefetcher types are picked up automatically);
                                  none attached = base
    kernels + order            <- the whitelist embedded in the binary (config.ini 'executable'),
                                  ordered by the RUN_KERNEL sequence in tsvc.c; falls back to
                                  the cmd= kernel filters, or --kernel for a single-kernel build
    per-kernel stats           <- OUTDIR/stats.txt split on the ROI dump sections

A single gem5 run of the whitelist binary writes one config + one stats.txt holding one
ROI section per kernel (plus a trailing cumulative dump). This splits that stats.txt per
kernel and copies the shared config files into each kernel's directory.

Examples:
    python tsvc-gem5/organize_results.py --size tiny out/base
    python tsvc-gem5/organize_results.py --size medium out/imp
    python tsvc-gem5/organize_results.py --size tiny --kernel s353 out/stride   # 1-kernel build
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime

# Fallback gem5 SimObject type (config.ini) -> result-tree prefetcher label, used only
# if the CLI registry in rvv/prefetcher_factory.py can't be read (see prefetcher_labels).
FALLBACK_PREFETCHER_TYPES = {
    "StridePrefetcher": "stride",
    "IndirectMemoryPrefetcher": "imp",
    "VectorIndirectMemoryPrefetcher": "vimp",
    "IrregularStreamBufferPrefetcher": "isb",
    "STeMSPrefetcher": "stems",
}

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
GEM5_ROOT = os.path.dirname(SCRIPT_DIR)  # tsvc-gem5/ -> gem5 root
COMMON_H = os.path.join(SCRIPT_DIR, "src", "common.h")
TSVC_C = os.path.join(SCRIPT_DIR, "src", "tsvc.c")
PREFETCHER_FACTORY = os.path.join(GEM5_ROOT, "rvv", "prefetcher_factory.py")
RESULTS_ROOT = os.path.join(GEM5_ROOT, "tsvc_results")


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def read_common_defs():
    """(iterations, LEN_1D, LEN_2D) from the first uncommented #define of each."""
    vals = {}
    pat = re.compile(r"^\s*#\s*define\s+(iterations|LEN_1D|LEN_2D)\s+(\d+)")
    with open(COMMON_H) as f:
        for line in f:
            if line.lstrip().startswith("//"):
                continue
            m = pat.match(line)
            if m and m.group(1) not in vals:
                vals[m.group(1)] = int(m.group(2))
    for k in ("iterations", "LEN_1D", "LEN_2D"):
        if k not in vals:
            die(f"could not find an uncommented '#define {k}' in {COMMON_H}")
    return vals["iterations"], vals["LEN_1D"], vals["LEN_2D"]


def prefetcher_labels():
    """gem5 SimObject class name -> result-tree label.

    Read from the PREFETCHERS dict in rvv/prefetcher_factory.py — the CLI
    name -> class registry every prefetcher must be added to before it can be
    selected with --prefetcher — so new prefetcher types are picked up here
    automatically. The factory imports m5.objects and so can't be imported
    outside gem5; parse the dict literal instead. Falls back to the static
    table if the registry can't be found.
    """
    labels = dict(FALLBACK_PREFETCHER_TYPES)
    try:
        text = open(PREFETCHER_FACTORY).read()
    except OSError:
        return labels
    m = re.search(r"^PREFETCHERS\s*=\s*\{(.*?)^\}", text, re.S | re.M)
    if not m:
        return labels
    for name, cls in re.findall(r'"([\w-]+)"\s*:\s*(\w+)', m.group(1)):
        if cls != "None":  # "none" maps to no prefetcher
            labels[cls] = name
    return labels


def parse_config(outdir):
    """(prefetcher_label, executable_path, cmd_args) from config.ini.

    The prefetcher is whatever type= appears in a [*.prefetcher] section (the
    cache attach point); no such section means no prefetcher was attached
    ("base"). An attached type missing from the rvv/prefetcher_factory.py
    registry gets a label derived from its class name, with a warning, rather
    than being silently filed under "base".
    """
    cfg = os.path.join(outdir, "config.ini")
    if not os.path.isfile(cfg):
        die(f"no config.ini in {outdir} — is it a gem5 output dir?")
    labels = prefetcher_labels()
    executable, cmd_args, section, attached = None, [], "", set()
    with open(cfg) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
            elif line.startswith("type=") and section.endswith(".prefetcher"):
                attached.add(line[len("type="):])
            elif line.startswith("executable="):
                executable = line[len("executable="):]
            elif line.startswith("cmd="):
                cmd_args = line[len("cmd="):].split()

    found = set()
    for typ in sorted(attached):
        label = labels.get(typ)
        if label is None:
            label = typ[: -len("Prefetcher")].lower() if typ.endswith(
                "Prefetcher") else typ.lower()
            print(
                f"warning: prefetcher type '{typ}' is not in the PREFETCHERS "
                f"registry of {os.path.relpath(PREFETCHER_FACTORY, GEM5_ROOT)}; "
                f"using derived label '{label}'",
                file=sys.stderr,
            )
        found.add(label)
    prefetcher = "+".join(sorted(found)) if found else "base"
    return prefetcher, executable, cmd_args


def tsvc_run_order():
    """Kernel names in RUN_KERNEL source order in tsvc.c (= the order they run)."""
    pat = re.compile(r"RUN_KERNEL\(\s*([A-Za-z0-9_]+)")
    order = []
    with open(TSVC_C) as f:
        for line in f:
            m = pat.search(line)
            if m:
                order.append(m.group(1))
    return order


def embedded_whitelist(binary, run_order):
    """Kernels baked in via -DTSVC_KERNELS, recovered from the binary, else None.

    The whitelist is one space-separated string literal; require >= 2 tokens so it is
    not confused with the individual kernel-name strcmp literals (each kernel name also
    appears alone in the binary).
    """
    if not binary:
        return None
    path = binary if os.path.isfile(binary) else os.path.join(GEM5_ROOT, binary)
    if not os.path.isfile(path):
        return None
    known = set(run_order)
    try:
        out = subprocess.run(
            ["strings", path], capture_output=True, text=True
        ).stdout
    except (FileNotFoundError, OSError):
        return None
    for line in out.splitlines():
        toks = line.split()
        if len(toks) >= 2 and all(t in known for t in toks):
            return toks
    return None


def determine_kernels(outdir, executable, cmd_args, forced_kernel):
    """(kernels_in_run_order, human_description_of_how_we_decided)."""
    run_order = tsvc_run_order()
    if forced_kernel:
        if forced_kernel not in run_order:
            die(f"--kernel {forced_kernel} is not a known TSVC kernel")
        return [forced_kernel], "forced via --kernel"

    wl = embedded_whitelist(executable, run_order)
    if wl:
        sel = set(wl)
        return [k for k in run_order if k in sel], "binary whitelist (-DTSVC_KERNELS)"

    # Runtime fallback: cmd = <binary> <kernel filters...>
    filt = {a for a in cmd_args[1:] if a in set(run_order)}
    if filt:
        return [k for k in run_order if k in filt], "cmd= kernel filters (--parms)"

    return run_order, "no filter — all kernels ran"


def split_stats(outdir):
    """List of stat-section strings (each Begin..End block, inclusive)."""
    stats = os.path.join(outdir, "stats.txt")
    if not os.path.isfile(stats):
        die(f"no stats.txt in {outdir}")
    text = open(stats).read()
    return re.findall(
        r"-+ Begin Simulation Statistics -+.*?-+ End Simulation Statistics\s+-+",
        text,
        re.S,
    )


def main():
    ap = argparse.ArgumentParser(
        description="Organize a gem5 TSVC run into the tsvc_results/ tree."
    )
    ap.add_argument("outdir", help="gem5 -d output directory (e.g. out/stride)")
    ap.add_argument(
        "--size",
        required=True,
        choices=["tiny", "small", "medium"],
        help="array-size label (you set the actual sizes in common.h at compile time)",
    )
    ap.add_argument(
        "--kernel",
        help="organize as this single kernel (for a 1-kernel KERNELS= build, where the "
        "whitelist can't be recovered from the binary)",
    )
    ap.add_argument(
        "--results-root",
        default=RESULTS_ROOT,
        help=f"destination tree root (default: {os.path.relpath(RESULTS_ROOT, GEM5_ROOT)})",
    )
    ap.add_argument(
        "--label",
        help="subdirectory name under iter_<N> for this run (default: current date-time). "
        "Keeps separate configurations from overwriting each other; pass a meaningful "
        "tag (e.g. vlen512_lane4) to identify a configuration.",
    )
    ap.add_argument(
        "--include-dot",
        action="store_true",
        help="also copy the graphviz config.dot/.pdf/.svg diagrams (large and identical "
        "across kernels; excluded by default)",
    )
    ap.add_argument(
        "--move",
        action="store_true",
        help="remove the source output dir after organizing (default: leave it in place)",
    )
    ap.add_argument(
        "--dry-run", action="store_true", help="print what would happen, write nothing"
    )
    args = ap.parse_args()

    outdir = os.path.abspath(args.outdir)
    if not os.path.isdir(outdir):
        die(f"{outdir} is not a directory")

    iterations, len1d, len2d = read_common_defs()
    prefetcher, executable, cmd_args = parse_config(outdir)
    kernels, ksource = determine_kernels(outdir, executable, cmd_args, args.kernel)
    sections = split_stats(outdir)

    # One label for the whole invocation, so every kernel from this run lands in the
    # same iter_<N>/<label> directory rather than overwriting a previous run.
    run_label = args.label or datetime.now().strftime("%Y-%m-%d_%H-%M-%S")

    print(f"size       : {args.size}")
    print(f"iterations : {iterations}  (LEN_1D={len1d}, LEN_2D={len2d}, from common.h)")
    print(f"prefetcher : {prefetcher}  (from config.ini)")
    print(f"run label  : {run_label}  (subdir under iter_{iterations})")
    print(f"kernels    : {len(kernels)} [{ksource}]: {' '.join(kernels)}")
    print(f"stat dumps : {len(sections)} ROI section(s) in stats.txt")

    if not sections:
        die(
            "stats.txt has no completed ROI sections — the run likely crashed before any "
            "kernel finished (e.g. the IMP gather panic). Nothing to organize."
        )

    n = min(len(kernels), len(sections))
    if len(sections) < len(kernels):
        print(
            f"warning: {len(sections)} stat sections for {len(kernels)} kernels — the run "
            f"may have crashed partway; organizing the first {n} (trailing kernels skipped)."
        )
    elif len(sections) > len(kernels):
        extra = len(sections) - len(kernels)
        print(
            f"note: {extra} extra section(s) beyond the {len(kernels)} kernels (final "
            f"cumulative dump) — ignored."
        )

    # config.ini etc. describe the whole run, so they are copied into every kernel dir;
    # only stats.txt is split per kernel.
    config_files = [
        f
        for f in sorted(os.listdir(outdir))
        if f != "stats.txt"
        and os.path.isfile(os.path.join(outdir, f))
        and (args.include_dot or not f.startswith("config.dot"))
    ]

    for i in range(n):
        kernel = kernels[i]
        dest = os.path.join(
            args.results_root, args.size, kernel, prefetcher, f"iter_{iterations}",
            run_label,
        )
        print(f"  {kernel:7s} -> {os.path.relpath(dest, GEM5_ROOT)}")
        if args.dry_run:
            continue
        os.makedirs(dest, exist_ok=True)
        with open(os.path.join(dest, "stats.txt"), "w") as f:
            f.write(sections[i].rstrip() + "\n")
        for fn in config_files:
            shutil.copy2(os.path.join(outdir, fn), os.path.join(dest, fn))
        with open(os.path.join(dest, "run_info.txt"), "w") as f:
            f.write(
                f"size={args.size}\nkernel={kernel}\nprefetcher={prefetcher}\n"
                f"iterations={iterations}\nLEN_1D={len1d}\nLEN_2D={len2d}\n"
                f"run_label={run_label}\nsource_outdir={outdir}\n"
            )

    if args.move and not args.dry_run:
        shutil.rmtree(outdir)
        print(f"removed source dir {os.path.relpath(outdir, GEM5_ROOT)}")
    print("done.")


if __name__ == "__main__":
    main()
