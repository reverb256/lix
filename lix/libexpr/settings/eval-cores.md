---
name: eval-cores
internalName: evalCores
type: unsigned int
default: 1
---
The number of cores to use for parallel evaluation. `0` means to use all
available cores (currently capped at 32). Parallel evaluation is opt-in:
the default of `1` keeps evaluation single-threaded, matching upstream
Determinate Nix, since parallel evaluation still has scalability
bottlenecks on some workloads.
