# Single-host page-server performance comparison

## Questions

Measure whether keeping memory images at the destination reduces source image
storage and I/O, and whether the identity checks add measurable overhead.
Measure final transfer and restore latency separately. Do not assume that fewer
source files or fewer protocol bytes implies lower application downtime.

## Existing evidence

Earlier route comparisons used read, splice and mixed pre-dump modes, with
plain and LZ4 payloads. The retrieved report summary gives roughly 38.48 MB of
plain transfer and 3.19 MB with LZ4 for its small workload. Once local final
pages and pagemaps were included, plain differences were about 1.8--4.5 KiB,
below 0.012%. Those observations do not establish a speed advantage.

The old report's local-final byte counts were lower bounds: file lengths did
not include copy-protocol overhead. Repeated CPU, freeze and end-to-end latency
measurements were missing. The report itself was not available in the current
Library access scope; these figures are historical context, not independently
rechecked measurements or acceptance thresholds.

## Comparisons

Use the clean candidate as the primary implementation. All controls must use
identical compiler options and the same dependency/kernel environment.

A. Candidate, matching remote parent: all pre-dump and final memory remote.
B. Same candidate, no reusable final parent: a full final transfer control.
   This isolates the value of incremental final reuse without a different tree.
C. Candidate versus a same-base build with only the read-buffer fix, in a
   deliberately valid, stable parent setup. Use this only to estimate the
   identity-check overhead; the legacy control is not a safety alternative.
D. Archived local-final implementation, only after its correctness gate passes
   the exact benchmark scenario. Include final file copying and all source
   memory metadata. Its different base and other changes are confounders; do
   not attribute every difference to the transport choice. A same-base port is
   required before making a causal implementation-cost claim.

Do not use the pre-fix read-mode implementation as a performance baseline.
Incorrect output cannot be traded for speed. Exclude failed restores from
latency summaries only with the failures reported alongside them; do not hide
failures by retrying until the requested sample count succeeds.

## Single-host setup

Run sender/workload and page-server/restore in separate processes and image
directories. Use TCP, rather than the correctness suite's Unix socketpair.
Reserve disjoint CPU sets where the runner permits it, and record the CPU model,
logical/physical topology, quota, kernel, filesystem and available memory.
Both sides still share memory bandwidth, caches, storage and the host scheduler.

Start with native loopback. If isolation and networking permissions are
available in a disposable CI VM, use two network namespaces connected by veth
and apply symmetric bandwidth/delay limits to that link only. Never change a
user machine's network settings for this experiment. Distinguish imposed RTT
from per-direction delay. Record actual measured throughput and RTT.

If shaping is unavailable, an explicitly instrumented TCP relay can model a
throughput cap, but it introduces extra copies, buffering and CPU cost. Report
it as a separate experiment, not as equivalent to a real cross-host network.

Keep source and destination storage modes equal across routes. Report warm
page-cache results explicitly. Do not globally drop caches on a shared runner.
Use isolated runner instances for a separate cold-cache/storage study, if
needed. Apparent file size and allocated blocks do not measure physical disk
writes; report them separately from process I/O counters.

## Workload and experiment stages

1. Screening: 256 MiB deterministic memory, three pre-dumps, five paired repeats,
   read and splice, plain payload, native loopback. Include a clean-page case
   and 1%/20% changed pages between rounds. Use identical seeds per pair.
2. Confirmation: retain informative conditions; expand to 1 GiB, plain/LZ4,
   compressible/pseudorandom page contents, and parent depth 1/3/8. Use ten paired
   repeats and publish the complete run list. Memory limits may require a
   smaller size, which must be stated rather than silently substituted.
3. Sensitivity: modest and constrained link profiles, for example 1 Gbit/s with
   1 ms RTT and 100 Mbit/s with 20 ms RTT, only when shaping can be verified.
4. Application check: one request-driven stateful service if the microbenchmark
   suggests a benefit. Measure request failures and longest client-observed
   service gap, not only CRIU's frozen interval.

For mechanism tests, mutate a predetermined set of pages between rounds and
pause mutation during capture to keep inputs equal. For realism, add a separate
continuous-dirtying case and measure achieved dirty rate; slower routes may
accumulate more dirty pages. Do not mix these two experiments in one aggregate.

Randomize or counterbalance route order within each pair. Warm up separately.
Do not run the benchmark alongside build jobs on the same runner. Publish each
run's durations and counters, paired differences, medians and uncertainty
intervals. Ten samples do not justify a reliable p99 estimate. Predeclare any
practical regression tolerance before reading the confirmation results.

## Measurements

- Per-round and total application-protocol bytes in both directions, including
  generation/query/control traffic. State whether TCP/IP headers are excluded.
- For local-final, measure actual final-copy traffic as well as pages/pagemap
  file sizes; include all pre-dump coverage metadata retained at the source.
- Source and destination apparent/allocated image bytes, process read/write
  counters, peak RSS, user/system CPU, and CPU time per GiB.
- Pre-dump elapsed time, CRIU-reported frozen time, final dump and transfer time,
  destination completion acknowledgment, restore time, and first successful
  external state verification or application request.
- Parent query/open counts and durations versus process count and chain depth,
  to expose metadata costs hidden by a large payload.

Measure an end-to-end interval directly on the same monotonic clock; do not add
phase durations that overlap. A final-copy route's local dump completion is
not its destination-ready time. If non-memory images are excluded for a narrow
memory-path metric, label it as such and also report an inclusive end-to-end
metric. Generation IDs and buffered flush acknowledgments are not fsync-based
durability guarantees.

## Correctness and stopping criteria

Each measured run must restore successfully and pass an external byte/state
check. Verify that matching incremental cases actually contain inherited pages,
that the full control does not, and that the candidate retains no source pages
or pagemaps. Log exact commit IDs, options, seeds, environment, kernel feature
probes and all failures/skips.

Stop and diagnose on a correctness failure. Complete screening before spending
on the larger matrix. If the confidence interval cannot distinguish the routes,
report that result rather than claiming equivalence. A single-host study can
establish local costs and controlled sensitivities; it cannot establish
cross-host speedups, independent storage effects or production migration SLA.
