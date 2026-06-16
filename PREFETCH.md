# Branch Structure

This branch adds two new features to the base gem5 Ara simulator: 
- Custom memory requests with additional fields for vector instructions (see `EXTENSIONS.md`)
- Configurable memory hierarchies differentiating between scalar and vector accesses (see `MEMORY_CONFIG.md`)

# Memory Hierarchy Configurations

## Isolated Vector Memory Hierarchy

This configuration is enabled with the `--scalar-uncacheable` flag. Disables caching for scalar accesses and redirects them to DRAM directly instead. See `MEMORY_CONFIG.md` for more details.

## Parallel Vector Memory Hierarchy

This configuration is enabled with the `--vector-cache` flag. Creates a separate `PrivateL1L2CacheHierarchy` object for vector accesses. See `MEMORY_CONFIG.md` for more details.

NOTE: The above configurations are meant to be used independently

# Building

```
make build PROG_DIR=<program_dir> PROG=<progam_executable> # default gem5 configuration
make build PROG_DIR=<program_dir> PROG=<progam_executable> UC=1 # isolated vector memory hierarchy
make build PROG_DIR=<program_dir> PROG=<progam_executable> VC=1 # parallel vector memory hierarchy
```

- Running tests with UC=1 or VC=1 will produce output products in two output directories: `m5out` for base gem5 config and `m5out2` for the isolated or parallel vector memory hierarchy config
- This was done to verify program outputs between base and modified configurations