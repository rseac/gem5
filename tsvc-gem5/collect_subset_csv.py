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
    
    # Identify subdirectories (could be configs or lanes)
    subdirs = [d for d in os.listdir(results_dir) if os.path.isdir(os.path.join(results_dir, d))]
    
    # Check if we have the new multi-config structure: results_dir/CONFIG/lanes_N/kernel/iter_I
    configs = [d for d in subdirs if d in ["TINY", "SMALL", "MEDIUM", "LARGE", "HUGE"]]
    
    if configs:
        for config in configs:
            config_path = os.path.join(results_dir, config)
            lane_dirs = [d for d in os.listdir(config_path) if d.startswith("lanes_")]
            for lane_dir in sorted(lane_dirs, key=lambda x: int(x.split("_")[1])):
                lanes = lane_dir.split("_")[1]
                lane_path = os.path.join(config_path, lane_dir)
                for kernel in sorted(os.listdir(lane_path)):
                    kernel_path = os.path.join(lane_path, kernel)
                    if not os.path.isdir(kernel_path): continue
                    process_kernel_iter(kernel, kernel_path, lanes, config, data)
    elif any(d.startswith("lanes_") for d in subdirs):
        # Middle structure: results_dir/lanes_N/kernel/iter_I
        lane_dirs = [d for d in subdirs if d.startswith("lanes_")]
        for lane_dir in sorted(lane_dirs, key=lambda x: int(x.split("_")[1])):
            lanes = lane_dir.split("_")[1]
            lane_path = os.path.join(results_dir, lane_dir)
            for kernel in sorted(os.listdir(lane_path)):
                kernel_path = os.path.join(lane_path, kernel)
                if not os.path.isdir(kernel_path): continue
                process_kernel_iter(kernel, kernel_path, lanes, "N/A", data)
    else:
        # Old structure: results_dir/kernel/iter_I
        for kernel in sorted(os.listdir(results_dir)):
            kernel_path = os.path.join(results_dir, kernel)
            if not os.path.isdir(kernel_path): continue
            process_kernel_iter(kernel, kernel_path, "N/A", "N/A", data)
                
    if not data:
        print("No data found to collect.")
        return

    csv_file = os.path.join(results_dir, "summary_results.csv")
    
    # Target headers: Kernel Iterations Lanes Array Config VLEN Cycles Instructions
    fieldnames = ["Kernel", "Iterations", "Lanes", "Array Config", "VLEN", "Cycles", "Instructions", "Validation"]
    
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
