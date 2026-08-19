---
name: parallel
args: [xs, x]
experimentalFeature: parallel-eval
---
Start evaluation of the values `xs` in the background and return `x`.

`xs` must be a list. Each element of `xs` that is not yet evaluated is
evaluated in the background, on the parallel-evaluation executor, while
`x` is evaluated normally; forcing `x` will wait for any elements it
depends on. The result of the function is the value of `x`.

Evaluation is pure, so the result is deterministic regardless of how the
background evaluation is scheduled.

```nix
builtins.parallel
  (map (n: builtins.trace "computing ${toString n}" (n * 2)) [ 1 2 3 ])
  "done"
```

evaluates to `"done"`, while the elements of the list are computed in
parallel.
