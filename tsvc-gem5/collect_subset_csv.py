#!/usr/bin/env python3
import os
import re
import csv
import sys

def load_golden(iters):
    golden = {}
    golden_file = f"input-small/checksum.golden.iter{iters}"
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
    
    # Iterate through kernel directories
    for kernel in sorted(os.listdir(results_dir)):
        kernel_path = os.path.join(results_dir, kernel)
        if not os.path.isdir(kernel_path):
            continue
            
        # Iterate through iteration directories
        for iter_dir in sorted(os.listdir(kernel_path)):
            if not iter_dir.startswith("iter_"):
                continue
            
            iters = iter_dir.split("_")[1]
            golden = load_golden(iters)
            
            log_path = os.path.join(kernel_path, iter_dir, "terminal.log")
            
            if not os.path.exists(log_path):
                continue
                
            with open(log_path, 'r') as f:
                content = f.read()
                
                # Extraction patterns
                # s000        	      153112	512066944.000000
                # s128	s128        	      455633	80000.000000
                arch_pattern = rf"^\s*{kernel}.*?(\d+)\s+([\d\.]+)\s*$"
                arch_match = re.search(arch_pattern, content, re.MULTILINE)
                
                # ROI Cycles (Loop):       85376
                roi_cycles_pattern = r"ROI Cycles \(Loop\):\s+(\d+)"
                roi_cycles_match = re.search(roi_cycles_pattern, content)
                
                # ROI Vector Insts:        6188
                roi_vec_pattern = r"ROI Vector Insts:\s+(\d+)"
                roi_vec_match = re.search(roi_vec_pattern, content)
                
                # Host Wallclock Time: 11.597 seconds
                wallclock_pattern = r"Host Wallclock Time:\s+([\d\.]+)"
                wallclock_match = re.search(wallclock_pattern, content)
                
                checksum = arch_match.group(2) if arch_match else "N/A"
                
                # Validation
                status = "N/A"
                if kernel in golden and checksum != "N/A":
                    try:
                        g_val = float(golden[kernel])
                        c_val = float(checksum)
                        # Use a small tolerance for float comparison
                        if abs(g_val - c_val) < 1e-5:
                            status = "PASS"
                        else:
                            status = "FAIL"
                    except ValueError:
                        status = "Error"

                row = {
                    "Kernel": kernel,
                    "Iterations": iters,
                    "Arch Cycles": arch_match.group(1) if arch_match else "N/A",
                    "ROI Cycles (Gem5)": roi_cycles_match.group(1) if roi_cycles_match else "N/A",
                    "ROI Vector Insts": roi_vec_match.group(1) if roi_vec_match else "N/A",
                    "Host Wallclock (s)": wallclock_match.group(1) if wallclock_match else "N/A",
                    "Checksum": checksum,
                    "Validation": status
                }
                data.append(row)
                
    if not data:
        print("No data found to collect.")
        return

    csv_file = os.path.join(results_dir, "summary_results.csv")
    keys = data[0].keys()
    with open(csv_file, 'w', newline='') as f:
        dict_writer = csv.DictWriter(f, fieldnames=keys)
        dict_writer.writeheader()
        dict_writer.writerows(data)
        
    print(f"Results successfully collected into {csv_file}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: ./collect_subset_csv.py <results_directory>")
        sys.exit(1)
    collect_results(sys.argv[1])
