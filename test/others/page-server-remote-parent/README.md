# Page-server remote-parent regression

This test covers [issue #2503](https://github.com/checkpoint-restore/criu/issues/2503): one or more page-server pre-dumps followed by a local final dump.

A remote pre-dump writes page payload to the page-server image chain and keeps a standard CRIU pagemap on the source. The source pagemap records parent coverage but has no local pages image. A later local dump accepts a parent reference only when the corresponding range is present in that pagemap. The assembled destination chain must restore successfully.

## Run

```sh
make -C test/others/page-server-remote-parent check
```

The targets can also be run separately:

```sh
make -C test/others/page-server-remote-parent PRE_DUMP_MODE=splice regression
make -C test/others/page-server-remote-parent PRE_DUMP_MODE=read regression
make -C test/others/page-server-remote-parent shared-regression
make -C test/others/page-server-remote-parent read-fault-regression
```

Set `KEEP_WORK_DIR=1` to keep images and logs from the shell regression.
The shared-memory regression always prints and keeps its work directory.

The regression covers a local incremental control, one- and two-round remote pre-dumps, page-generation placement, restore, metadata rollback after image-write failures, and page-server disconnects.

The shared-memory regression verifies restored bytes and aliasing with `CRIU_TRACK_SHMEM` unset, so shared contents follow the default conservative copy path.

The read-fault regression uses a client-only syscall shim to exercise real
short reads, first-iovec and partial-iovec `EFAULT`, terminal recovery errors
(`EPERM`, `ESRCH`, and zero progress), and partial/failed `vmsplice` calls.
It checks exact skipped/recovered coverage and final page placement, then
verifies all tested private and shared bytes after restore. An unread page
is explicitly changed after pre-dump before requiring final recapture; missing
coverage alone does not make a clean page soft-dirty. Failed rounds recover
through a fresh full baseline. Every injection must report its execution,
and the suite retains its work directory and per-case JSON results.

### Recovery after a failed pre-dump

Source coverage rollback is not soft-dirty-state rollback. A failed pre-dump
may already have cleared soft-dirty bits even though it did not publish its
coverage metadata. Retrying incrementally against an earlier successful
baseline can then inherit stale pages. That retry is unsafe and is not fixed
by this change.

The supported recovery tested here is a full pre-dump, without
`--prev-images-dir`, into fresh source and destination directories. Only after
that full pre-dump succeeds does the final incremental dump use the new
baseline. The regression injects a second-round source inventory-write
`ENOSPC` after a successful receiver transfer, checks that the old source and
destination image files are unchanged, then performs this full retry and
restores the assembled final-to-retry chain. It verifies the entire 33 MiB
patterned workload range with an external byte oracle before resetting it;
the workload's own check covers its remaining memory regions.

For a separate diagnostic of the unsafe old-baseline incremental retry:

```sh
CRIU_TEST_UNSAFE_INCREMENTAL_RETRY=1 make -C test/others/page-server-remote-parent PRE_DUMP_MODE=splice regression
CRIU_TEST_UNSAFE_INCREMENTAL_RETRY=1 make -C test/others/page-server-remote-parent PRE_DUMP_MODE=read regression
```

This diagnostic is intentionally excluded from default passing coverage and
is expected to fail when it detects stale generation placement. It does not
establish support for retrying from an older baseline after failure.

### THP coverage

The shared-memory regression requests transparent huge pages (THP) for one
variant. It reads `AnonHugePages` from `/proc/PID/smaps` for both processes
immediately before each pre-dump and the final dump. Only huge-page bytes
attributable to the tested private address range count. If a VMA extends
outside that range, its outside bytes are discounted from the counter,
making the reported `private_huge_kb` a conservative lower bound.

A THP PASS requires a positive count in both tested private mappings at all
three captures, including the incremental captures after COW writes.
The per-case `result.json` and aggregate `results.json` retain these samples.
If that evidence is missing, the THP variant reports `status: SKIP` and
`thp_status: SKIP`, while `bytes_status: PASS` separately records successful
restored-byte and sharing checks. The suite summary also reports the THP skip;
THP being requested or observed elsewhere in the process is not a THP pass.

To require THP coverage rather than allow a skip, run as root:

```sh
python3 test/others/page-server-remote-parent/shared_regression.py --require-thp
```

This exits unsuccessfully if THP evidence is missing, after saving the results.
The samples establish THP presence just before capture; they do not guarantee
that the kernel will retain huge-page backing throughout CRIU's capture.

### Auto-dedup coverage and failure limits

The shared regression's `splice-plain-base-dedup` variant enables
`--auto-dedup` only on the pre-dump clients and the final local dump. The
page-server receivers do not receive that option. This checks that source-side
coverage-only parents are not treated as local payloads for deduplication;
it does not test receiver-side auto-dedup or its failure recovery.

Receiver-side `--auto-dedup` can destructively hole-punch older destination
parent payloads before a later receive or image-write failure. Rolling back
new source coverage metadata does not undo those changes. There is no
transactional rollback or guarantee that older destination parents remain
independently restorable after such a failure. Keep an independent copy of
any parent chain that must survive receiver-side deduplication.

### Parent-chain identity

Coverage files are source-side dump metadata, not restorable destination
parents. The suite also checks that accidentally configuring a page server
with such a parent fails without publishing the next round's coverage.
The ordinary payload reader rejects their reserved `pages_id = 0` marker.
The caller's existing identity contract is unchanged: it must pair source
coverage and destination payloads from the correct process instance and
generation, and assemble the correct parent chain. Range coverage and receiver
validation do not bind the two chains to the same contents. Cryptographic or
generation-identity binding is outside this feature's scope.
