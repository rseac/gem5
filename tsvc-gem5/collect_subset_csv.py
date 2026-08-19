#!/usr/bin/env python3
import os
import re
import csv
import sys

def load_golden(iters, config):
    golden = {}
    # Map config to directory (e.g. TINY -> input-tiny)
    config_dir = f"input-{config.lower()}"
    golden_file = os.path.join(config_dir, f"checksum.golden.iter{iters}")
    if os.path.exists(golden_file):
        with open(golden_file, 'r') as f:
            for line in f:
                # Format: risky  s000          0.000      471923584.000000
                match = re.search(r"^\s*risky\s+(\w+)\s+[\d\.]+\s+([\d\.]+)", line)
                if match:
                    golden[match.group(1)] = match.group(2)
    return golden

def collect_results(results_dir):
    data = []
    
    # Recursively find all terminal.log files
    for root, dirs, files in os.walk(results_dir):
        if "terminal.log" in files:
            # Path structure typically: results_dir/[CONFIG]/[vlen_V]/[lanes_L]/kernel/iter_I
            parts = os.path.relpath(root, results_dir).split(os.sep)
            config = "N/A"
            lanes = "N/A"
            vlen_dir = "N/A"
            kernel = "N/A"
            iters = "1"
            
            for part in parts:
                if part in ["TINY", "SMALL", "MEDIUM", "LARGE", "HUGE"]:
                    config = part
                elif part.startswith("lanes_"):
                    lanes = part.split("_")[1]
                elif part.startswith("vlen_"):
                    vlen_dir = part.split("_")[1]
                elif part.startswith("iter_"):
                    iters = part.split("_")[1]
                else:
                    kernel = part
                    
            golden = load_golden(iters, config)
            log_path = os.path.join(root, "terminal.log")
            # Check if stats.txt exists in m5out
            stats_path = os.path.join(root, "m5out", "stats.txt")
            roi_insts = "N/A"
            roi_ipc = "N/A"
            
            if os.path.exists(stats_path):
                try:
                    with open(stats_path, 'r') as sf:
                        scontent = sf.read()
                    blocks = scontent.split("---------- Begin Simulation Statistics ----------")
                    if len(blocks) > 1:
                        # Block 1 is ROI
                        sim_inst_m = re.search(r"simInsts\s+(\d+)", blocks[1])
                        ipc_m = re.search(r"board\.processor\.cores\.core\.ipc\s+([\d\.]+)", blocks[1])
                        if sim_inst_m: roi_insts = sim_inst_m.group(1)
                        if ipc_m: roi_ipc = f"{float(ipc_m.group(1)):.4f}"
                except Exception:
                    pass
                    
            with open(log_path, 'r') as f:
                content = f.read()
                
                roi_cycles_match = re.search(r"ROI Cycles \(Loop\):\s+(\d+)", content)
                roi_vec_match = re.search(r"ROI Vector Insts:\s+(\d+)", content)
                vlen_match = re.search(r"VLEN=(\d+)", content)
                e2e_match = re.search(r"Total Execution Cycles:\s+(\d+)", content)
                
                vlen = vlen_match.group(1) if vlen_match else (vlen_dir if vlen_dir != "N/A" else "N/A")
                
                arch_match = re.search(rf"^\s*{kernel}.*?(\d+)\s+([\d\.]+)\s*$", content, re.MULTILINE)
                checksum = arch_match.group(2) if arch_match else "N/A"
                
                status = "N/A"
                if kernel in golden and checksum != "N/A":
                    try:
                        g_val = float(golden[kernel])
                        c_val = float(checksum)
                        if abs(g_val - c_val) < 1e-5:
                            status = "PASS"
                        else:
                            status = "FAIL"
                    except ValueError:
                        status = "Error"
                        
                row = {
                    "Kernel": kernel,
                    "Iterations": iters,
                    "Lanes": lanes,
                    "Array Config": config,
                    "VLEN": vlen,
                    "ROI Insts": roi_insts,
                    "ROI Vector Insts": roi_vec_match.group(1) if roi_vec_match else "N/A",
                    "ROI Cycles": roi_cycles_match.group(1) if roi_cycles_match else "N/A",
                    "ROI IPC": roi_ipc,
                    "E2E Cycles": e2e_match.group(1) if e2e_match else "N/A",
                    "Validation": status
                }
                data.append(row)
                
    if not data:
        print("No data found to collect.")
        return

    csv_file = os.path.join(results_dir, "summary_results.csv")
    fieldnames = ["Kernel", "Iterations", "Lanes", "Array Config", "VLEN", "ROI Insts", "ROI Vector Insts", "ROI Cycles", "ROI IPC", "E2E Cycles", "Validation"]
    
    # Sort data for clean presentation: Config -> VLEN -> Lanes -> Kernel -> Iterations
    def sort_key(r):
        cfg_order = {"TINY": 1, "SMALL": 2, "MEDIUM": 3, "LARGE": 4, "HUGE": 5}.get(r["Array Config"], 99)
        v = int(r["VLEN"]) if r["VLEN"].isdigit() else 0
        l = int(r["Lanes"]) if r["Lanes"].isdigit() else 0
        i = int(r["Iterations"]) if r["Iterations"].isdigit() else 0
        return (cfg_order, v, l, r["Kernel"], i)
        
    data.sort(key=sort_key)
    
    with open(csv_file, 'w', newline='') as f:
        dict_writer = csv.DictWriter(f, fieldnames=fieldnames)
        dict_writer.writeheader()
        dict_writer.writerows(data)
        
    print(f"Results successfully collected into {csv_file}")

def process_kernel_iter(kernel, kernel_path, lanes, config, data):
    # Iterate through iteration directories
    for iter_dir in sorted(os.listdir(kernel_path)):
        if not iter_dir.startswith("iter_"):
            continue
        
        iters = iter_dir.split("_")[1]
        golden = load_golden(iters, config)
        
        log_path = os.path.join(kernel_path, iter_dir, "terminal.log")
        
        if not os.path.exists(log_path):
            continue
            
        with open(log_path, 'r') as f:
            content = f.read()
            
            # Extraction patterns
            roi_cycles_pattern = r"ROI Cycles \(Loop\):\s+(\d+)"
            roi_cycles_match = re.search(roi_cycles_pattern, content)
            
            roi_vec_pattern = r"ROI Vector Insts:\s+(\d+)"
            roi_vec_match = re.search(roi_vec_pattern, content)
            
            # Extract VLEN from the "Running..." line: L1D=32KiB, L2=512KiB, VLEN=2048, ...
            vlen_pattern = r"VLEN=(\d+)"
            vlen_match = re.search(vlen_pattern, content)
            vlen = vlen_match.group(1) if vlen_match else "N/A"
            
            # For validation
            arch_pattern = rf"^\s*{kernel}.*?(\d+)\s+([\d\.]+)\s*$"
            arch_match = re.search(arch_pattern, content, re.MULTILINE)
            checksum = arch_match.group(2) if arch_match else "N/A"
            
            status = "N/A"
            if kernel in golden and checksum != "N/A":
                try:
                    g_val = float(golden[kernel])
                    c_val = float(checksum)
                    if abs(g_val - c_val) < 1e-5:
                        status = "PASS"
                    else:
                        status = "FAIL"
                except ValueError:
                    status = "Error"

            row = {
                "Kernel": kernel,
                "Iterations": iters,
                "Lanes": lanes,
                "Array Config": config,
                "VLEN": vlen,
                "Cycles": roi_cycles_match.group(1) if roi_cycles_match else "N/A",
                "Instructions": roi_vec_match.group(1) if roi_vec_match else "N/A",
                "Validation": status
            }
            data.append(row)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: ./collect_subset_csv.py <results_directory>")
        sys.exit(1)
    collect_results(sys.argv[1])
