# Issue triage for the Rosetta era

Triage of the 128 open issues after the QEMU/TCG engine was removed (#410). Closed issues carry a comment with the reason.

| Outcome | Count |
|---|---|
| Closed: obsolete (QEMU/TCG or core/cpu only) | 8 |
| Closed: fixed on main | 9 |
| Kept open, labelled `rosetta` | 105 |
| Untouched (upstream sync #415-#421) | 6 |

Kept issues whose text pointed at core/cpu or QEMU/TCG got a comment naming the current location.

## Closed as obsolete

Reason: the QEMU/TCG engine was removed in #410; the Rosetta direct path replaces it.

- #175 [XT-02] Unbounded GPU WaitIdle on the AppKit/guest thread for every mapping change deadlocks when the GPU waits (WAIT_REG_MEM) on a CPU-written value
- #194 [BT-09] 20 tests (VideoOut, Flip, UpstreamCorrectness) are reachable only through an undocumented diagnostic wrapper
- #195 [BT-10] Guest CPU runtime / native runner build and its host prerequisites are undocumented
- #203 [CPU-26] No tests for capacity limits, RDTSC, context churn, slot exhaustion, starvation or throughput
- #209 [GAP-17] Guest thread stacks are fixed at 1 MiB and scePthreadAttrSetstacksize is not provided
- #210 [GAP-18] Every guest mapping change drains the GPU, and every TCG map/unmap flushes all translated code
- #224 [MEM-25] AppKit main thread is pumped every 4096 guest instructions but stalls completely during long host waits
- #253 [Audit] Low/info findings: cpu

## Closed as fixed

- #124 [AGC-03] Every Metal draw and dispatch allocates new MTLBuffers and memcpys all borrowed guest memory (fixed by #383, #426)
- #126 [GAP-08] Each Metal draw copies all guest memory, recompiles shaders from source, builds a new pipeline and waits for the GPU (fixed by #383, #357, #427)
- #127 [MEM-04] Native VideoOut exposes only 7 services: no SubmitFlip, flip/vblank events, GetFlipStatus, WaitVblank or IsFlipPending (fixed by #397)
- #130 [AGC-02] No Metal shader or pipeline caching: every draw re-runs SPIR-V to MSL, compiles MSL from source and builds a PSO (fixed by #357, #427)
- #136 [AGC-09] Metal compute dispatches cannot see depth/stencil surfaces (fixed by #401)
- #151 [MEM-01] Guest memory is copied into new MTLBuffers for every draw and dispatch instead of being aliased (no newBufferWithBytesNoCopy) (fixed by #383)
- #155 [MSL-01] Metal: every draw and dispatch re-runs SPIRV-Cross, MSL compile and pipeline creation, then blocks on the GPU (fixed by #357, #427)
- #166 [SEC-03] Every Metal draw and dispatch copies all GPU-visible guest memory into new MTLBuffers (guest-controlled per-draw cost / failure) (fixed by #383)
- #170 [SHD-05] Centroid and per-sample interpolation are always rejected; no Centroid/Sample decoration is emitted (and f16 interpolation needs barycentrics) (fixed by #358)
