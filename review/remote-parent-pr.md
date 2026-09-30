# Bind remote incremental memory images to their parent generation

## Problem

With pre-dump and final memory sent through a page-server, the source needs to
know whether the destination has the parent generation it expects. A matching
PID, directory name or address range is not enough. Reusing a different parent
can leave an incremental image with the wrong dependency.

## Changes

1. Preserve read-mode pre-dump buffer pages while the pipe or socket still owns
   them. This fix is independent of parent identity.
2. Identify each dump's memory generation in the inventory and pagemap headers.
   Check the destination parent before pre-dump omits clean pages, then check
   the reader actually opened for image output. Reject inherited ranges if that
   parent is no longer usable. Validate inherited dependencies again on restore;
   do not require unused ancestors for a self-contained image.
3. Add regression tests for read and splice modes, matching and mismatched
   generations, legacy parent headers, a broken unused ancestor, and parent
   replacement between query and open. Restore and check memory externally.

This uses the existing page-server and pagemap paths. It does not introduce a
source-side coverage image or retain source memory payloads. Non-memory final
images must still be transferred by the migration caller.

## Compatibility and failure handling

Generation-aware commands require matching sender and receiver support. Older
commands keep their existing semantics; an unsupported new command must fail.
Legacy images without generation metadata retain their restore behavior.

The early query does not reserve an image. If the parent changes after clean
pages have been omitted, an inherited write fails. The caller must discard that
failed iteration and arrange source-process retention and retry. A generation
identifier is not a checksum, an immutable snapshot, or a durability guarantee.

## Scope relative to issue #2503 and PR #3150

This candidate targets a coordinated migration with a destination page-server
available for both pre-dump and final dump. It does not implement a final local
dump that can finish independently of the destination. That workflow remains a
separate archived implementation, rather than an implicit promise of this PR.

## Review and validation

The series is based on 3e067bc2639b7f98e8bc64b30c05f60b4dd52df8. Production
sources and regression test logic are byte-identical to the previously tested
6d9c6775dd32b3d904d093ec05fdbbd28051c1a9 tree. The sequence and commit messages
have been rewritten; unrelated CI maintenance is excluded.

Fresh build and test results belong to the new commit IDs. The companion
validation branch carries the before/after reproduction runner, per-commit
build checks, and CI infrastructure adjustments. Do not confuse a skipped
kernel-dependent test with a passing memory migration test.

Before updating the upstream PR, the contributor must review and add the
project's required DCO sign-offs. This draft does not add a certification on
the contributor's behalf. It has not been posted to the upstream PR.
