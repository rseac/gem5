# The gem5 Simulator (ARA Vector Timing Model Extension)

This repository is a modified fork of [gem5](https://www.gem5.org/) that models the **ARA RISC-V Vector Architecture** timing behaviour, including dynamic operation latency, vector chaining, and multi-cluster support for **AraXL**.

## ARA Timing Model

The model replaces gem5's default 1-cycle vector latencies with values derived from the ARA RTL. 

### Versions and Configuration

You can simulate two versions of the ARA hardware by toggling parameters:

#### 1. Standard ARA (Single Cluster)
Models the base ARA design with a single cluster of lanes.
*   **Throughput**: Set `--vector-timing-throughput` to the hardware lane count (2, 4, 8, or 16).
*   **Example**:
    ```bash
    build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py \
        --vlen 1024 --vector-timing-throughput 4 \
        /path/to/binary
    ```

#### 2. AraXL (Multi-Cluster)
Models the extra-large ARA design with multiple clusters connected via a ring interconnect.
*   **Clusters**: Set `--is-araxl` and `--nr-clusters` (e.g., 2, 4, 8).
*   **Latency**: Set `--ring-latency` to model inter-cluster hop delay (default 2).
*   **Throughput**: `--vector-timing-throughput` models lanes **per cluster**.
*   **Example**:
    ```bash
    build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py \
        --is-araxl --nr-clusters 4 --ring-latency 2 \
        --vlen 4096 --vector-timing-throughput 4 \
        /path/to/binary
    ```

---

### Key Parameters

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `--is-araxl` | off | Enable AraXL multi-cluster timing characteristics |
| `--nr-clusters` | 1 | Number of ARA clusters (Requires `--is-araxl`) |
| `--ring-latency` | 2 | Cycles per cluster hop in AraXL ring interconnect |
| `--vlen` | 512 | Vector register length in bits |
| `--vector-timing-throughput` | 4 | **Number of lanes per cluster** |
| `--simd-units` | 2 | FU slots for chaining — **Always keep at 2** |
| `--enable-chaining` | off | Enable vector compute and load chaining |

---

## Running ARA Simulations

Use `rvv/riscv-rvv-se-ara.py` as the simulation script:

```bash
build/RISCV/gem5.opt rvv/riscv-rvv-se-ara.py [FLAGS] /path/to/riscv-binary
```

For full parameter documentation and detailed latency tables, see [README_ARA_sim.md](README_ARA_sim.md).

---

## Original gem5 Repository Information

This is the repository for the gem5 simulator. It contains the full source code
for the simulator and all tests and regressions.

The main website can be found at <http://www.gem5.org>.
