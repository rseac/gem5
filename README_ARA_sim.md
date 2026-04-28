# ARA Vector Simulation in gem5

This guide describes the custom ARA timing model implemented in this gem5 fork, including vector chaining, the functional unit pool, and the multi-cluster **AraXL** extension.

---

## Model Versions

The gem5 ARA model supports two architectural variants:

### 1. Standard ARA (Single Cluster)
This models the baseline ARA hardware. All lanes reside in a single cluster with shared functional units and a unified AXI interface.

**Configuration:**
*   `--vector-timing-throughput`: Set to total lane count (2, 4, 8, or 16).
*   `--is-araxl`: Do not set (defaults to `False`).
*   `--nr-clusters`: Defaults to 1.

### 2. AraXL (Multi-Cluster)
This models an "extra-large" configuration where lanes are distributed across multiple clusters connected by a **Ring Interconnect**. It accounts for the latency of inter-cluster communication and additional pipeline stages.

**Configuration:**
*   `--is-araxl`: Enables the multi-cluster model.
*   `--nr-clusters`: Set to the number of clusters (2, 4, 8, ...).
*   `--vector-timing-throughput`: Set to the number of lanes **per cluster**.
*   `--ring-latency`: (Optional) Cycles per ring hop (default 2).

#### AraXL Timing Characteristics
When `--is-araxl` is enabled:
1.  **Inter-Cluster Overhead**: Reductions (`vfredsum`) incur logarithmic overhead, and slides/permutations incur linear distance-based overhead.
2.  **Increased Memory Latency**: Load/Store address generation increases from 3 to 6 cycles to model multi-cluster shuffle/align logic.
3.  **Front-end Handshake**: Sequencer dispatch floor increases by 2 cycles to model extra inter-cluster synchronization registers.
4.  **Core Scaling**: Core resources (ROB, registers, fetch width) automatically scale with `--nr-clusters` to support the wider system.

---

## Comparison Table

| Feature | Standard ARA | AraXL |
| :--- | :---: | :---: |
| `is_araxl` Flag | `False` | `True` |
| Lanes | `--vector-timing-throughput` | `nr_clusters * throughput` |
| Load AGU Latency | 3 cycles | 6 cycles |
| Reduction Latency | 1 cycle | 1 + Cluster Tree Overhead |
| Permute Latency | 1 cycle | 1 + Ring Distance Overhead |
| Dispatch Floor | `max(4, 12-L)` | `max(4, 12-L) + 2` |

---

## Parameter Guide

### `--vector-timing-throughput`
Controls the processing rate of vector instructions.
*   **Standard ARA**: Set this to the total lanes.
*   **AraXL**: Set this to the lanes **per cluster**.

### `--simd-units` (Always keep at 2)
Controls gem5 O3 FU slots. **Always use 2**.
*   This provides one slot for the producer and one for the consumer, which is the minimum required for gem5's `WakeDependents` chaining mechanism to function.

### `--nr-clusters`
Defines the number of ARA clusters in the AraXL model.
*   Scales inter-cluster latency and core buffer sizes.
*   Has no effect unless `--is-araxl` is also provided.

---

## Building and Running

```bash
# Build
scons build/RISCV/gem5.opt -j$(nproc)

# Run Standard ARA (4 lanes)
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py \
    --vlen 512 --vector-timing-throughput 4 \
    /path/to/binary

# Run AraXL (4 clusters, 4 lanes each = 16 total lanes)
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py \
    --is-araxl --nr-clusters 4 --vector-timing-throughput 4 \
    /path/to/binary
```
