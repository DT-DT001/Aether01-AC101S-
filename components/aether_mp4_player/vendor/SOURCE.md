# minimp4 provenance

Upstream: https://github.com/lieff/minimp4

Pinned commit: `2a3014a7753cfcba4442adeb469431967238fe6a` (2026-09-25).

License: CC0-1.0, retained in the header. Only the demultiplexer is used.

Original upstream header SHA256:
`9d122076cc23e5f4a7560ce4056c7d84484a8b8bf094d27c8f652f11fc725ce7`.

Local changes retain the upstream API and add the following demux safeguards:

- Bound the `avcC` payload and SPS/PPS lengths before copying; reject zero-length
  or truncated parameter sets and invalid NAL-length prefixes.
- Retain AVC profile, constraints, level and NAL-length-prefix size for format
  checks and conversion to Annex B.
- Record nonzero composition offsets so this player can reject reordered video.
- Parse a single normal-speed `elst` edit, including AAC encoder priming; mark
  complex, empty or negative-time edits unsupported.
- Make `MINIMP4_MAX_ALLOC_BYTES` overridable. The wrapper supplies PSRAM
  allocation accounting (2MiB per allocation, 8MiB total), cached reads,
  cancellation, bounded parsing work and media-format checks.
- Reuse the complete current sample-to-chunk interval as well as the next-chunk
  resume point. Multiple AAC access units in one chunk no longer restart the
  chunk-table scan from zero after a seek. No extra index allocation or public
  API change is required; backward/random queries retain the checked fallback.

The upstream header already contains hardening of sample tables, descriptor
lengths and allocation failures. The player additionally tests malformed
`avcC`, truncated containers and deterministic metadata mutations under
AddressSanitizer and UndefinedBehaviorSanitizer. This is regression coverage,
not a claim that all possible malformed containers have been exhaustively
validated.
