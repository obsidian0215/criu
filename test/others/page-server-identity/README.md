# Page-server parent identity

Run `make run` as root after building CRIU. The test needs the same kernel
features as the regular dump/restore suite. It keeps its logs and image
chains in `work/`; remove that directory before starting another run.

The source sends both pre-dump and final memory to a page-server. The test
asserts that no source pages or pagemaps remain, copies only non-memory
final images, restores the target, and checks the workload's bytes from a
separate process.

Cases cover matching, mismatching and legacy parent headers in both
pre-dump modes; full final fallback with a broken ancestor; and a parent
changed between the pre-dump query and image open. The last case must fail
rather than acknowledge inherited pages without a usable parent.

The parent query checks identity before clean pages are omitted. It does
not reserve an image: image open checks again, and a later mismatch fails
any inherited write. Callers must discard failed iterations and retain a
usable source process until the destination is ready. A generation ID is
not a content checksum or a durability guarantee.

The generation-aware commands require matching sender and receiver
support. Older commands retain their legacy semantics. An unknown command
must fail rather than silently downgrade identity checking.
