---
name: parallel-eval
internalName: ParallelEval
---
Enable built-in functions for parallel evaluation.

This enables `builtins.parallel`, which starts evaluation of a list of
values in the background on the parallel-evaluation executor, and
parallel deep forcing of values when printing structured output
(`nix eval --json`, `nix flake show`).

Parallel evaluation is controlled by the `eval-cores` setting
(`--eval-cores N` on the command line); `0` (the default) uses all
available cores, up to a limit of 32. Setting `eval-cores = 1` disables
worker threads entirely.
