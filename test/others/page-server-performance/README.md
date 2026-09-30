# Direct all-page-server comparison

This harness compares the exact unchanged base `3e067bc2639b7f98e8bc64b30c05f60b4dd52df8`
and candidate `ef3736b245a10bbb90c4ce9bf1295e57c1f7abb1`. Both send every pre-dump and final
memory image directly through TCP loopback to a same-version page server. Source
parents retain inventory, without source memory payloads or pagemaps. Final non-memory
images are copied locally before restoration. No LZ4 compression is enabled.

The base already asks the page server whether a parent exists. Therefore this valid,
stable-parent comparison measures candidate overhead/cost differences, not a promised
reduction in memory transfer. The candidate adds identity validation and safe fallback.
A separate candidate-full-final route omits the final parent on both sender and receiver;
it measures incremental-final reuse, not an improvement over unmodified CRIU.

Run `check-ci.sh` in the project's privileged Alpine test container. Place these sources
under `test/others/page-server-performance/`, and the workflow under `.github/workflows/`.
The initial 16 MiB smoke restores both variants. The bounded screen runs 256 MiB,
three pre-dumps plus a final dump, five paired repetitions, and 0%, 1%, 20% dirtying.
Each route first gets a separately labeled warm-up. Pair order is counterbalanced.
Only splice is selected: unmodified read mode must pass its own correctness gate
before any read-mode performance comparison. The harness also supports read explicitly.

Every run starts a fresh deterministic anonymous-memory process. Before each later
round the workload rewrites exactly the first floor(page_count * dirty_percent / 100)
pages, acknowledges completion, then stays idle during capture. Thus the measured
memory workload is controlled rather than continuously dirtying. Runtime stack/libc
pages remain normal process overhead. The idle process polls a signal flag with a 1 ms
sleep, adding a small, constant runtime CPU/stack-dirtying cost to every route. Every final generation is restored; an external
process reads all workload memory and checks its SHA-256 against independently built
expected bytes. Incremental rounds must contain inherited pages; full controls must
not. Failed cases stop the entire experiment and remain in results.jsonl.

Raw evidence includes every attempted run, exact revisions, build logs, environment,
per-round source/destination apparent and allocated image sizes, page-payload sizes,
CRIU stats, child CPU and output-block accounting, sender-return and destination-ready
timings, restore latency, and final-start-to-external-verification latency. The last
includes metadata copy and full hash verification and is not application downtime.
CRIU's own frozen interval is retained in stats. Child CPU includes sender and receiver;
the interval starts immediately before sender launch, after receiver startup. Resource
output blocks do not equal durable storage bytes. Peak RSS is not recorded because
RUSAGE_CHILDREN high-water accounting cannot attribute per-round maxima correctly.

TCP_INFO records data octets sent in both directions on the actual endpoint sockets,
including CRIU control traffic but excluding TCP/IP headers; retransmission counters
are reported separately. This uses a compiled helper against Linux headers, with a
runtime returned-structure-length check, rather than guessed Python ABI offsets. Storage uses normal warm caches without fsync/drop-caches.
Both endpoints share CPU, storage and memory bandwidth. There is no CPU pinning, traffic
shaping, cross-host claim, cold-storage claim, or standalone identity-only attribution:
the candidate also includes correctness/flush changes. The summary provides all values,
medians and paired differences, with observed ranges rather than misleading p99 or
confidence/equivalence claims from five observations. The screening result should guide
whether a larger and more isolated experiment is warranted.
