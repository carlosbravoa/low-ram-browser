# adblock-rust patches

Against https://github.com/brave/adblock-rust at `6028a6d` (version 0.13.3,
2026-09-29). Apply with `git am` in that order.

- `0001` `Engine::deserialize_static`: use the serialized engine in place
  from memory that stays mapped for the process (a read-only file shared by
  every lrb instance) instead of copying it. Always verifies; rejects
  unaligned data. Private memory per process with the full lists: 7.8 MB
  copied, 0.8 MB in place (MEASUREMENTS.md).
  Generic enough to propose upstream.
