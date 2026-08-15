---
synopsis: "Check determinism of fixed-output derivations and run the diff hook in `--check` mode"
category: "Improvements"
---

`nix build --check` previously skipped the determinism comparison for
fixed-output derivations (the only content-addressed derivations Lix
supports, since floating content-addressed derivations are rejected): a
rebuild that produced different content was only reported as a hash
mismatch.

`--check` now applies the same treatment to fixed-output derivations as
to input-addressed ones.  A rebuild that matches the declared hash is
compared against the previously registered output and marked as
ultimately trusted, and a rebuild that produces different content runs
the configured diff hook (`--diff-hook`) against the previously
registered output at the declared-hash path, so the difference can be
inspected alongside the hash-mismatch error.
