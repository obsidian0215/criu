Review-only local-final patch partition

Base: 3e067bc2639b7f98e8bc64b30c05f60b4dd52df8
Validated combined candidate: 67eb051014b788f16600782c86cdd92a644d1040

0001: isolated read-buffer repair
0002: local-final feature and unit tests
0003: normal integration regression and ordinary CI integration

validation-only-supplement.patch restores focused workflow, baseline comparison
options, runc fixtures, and validation documentation. The stage checker applies
it ONLY to a separate temporary index for whole-tree identity verification.
It does not enter any of the three stage builds.

Run in the standard privileged Alpine image with:
  CC=gcc HOSTCC=gcc bash scripts/ci/local-final-review-series.sh
  CC=clang HOSTCC=clang bash scripts/ci/local-final-review-series.sh

The existing focused workflow selects this runner for branch names beginning
validation/local-final-series- or a workflow_dispatch with review_series=true.
Other branches keep the original focused runner.

Each compiler gets three clean builds and three unit-test runs. GCC additionally
builds the test workloads and runs the normal splice regression once to exercise
the integration wrapper after removal of validation-only options. It does not
repeat the already-passed full read/shared/runc/baseline runtime matrices.

The runner applies the patches to a detached worktree without commits or
external ref writes. Exact indexed trees are checked after each patch. After
stage 3, all production and unit-test blobs must match the validated candidate.
Adding the validation supplement in the separate index must reproduce the
complete validated tree, including all file modes and unchanged files:
  f9946ae4a0bf9e5efde8e3d10af3942b10a0ab23

Evidence is written beneath .local-final-ci/review-series and included by the
existing workflow archive step. It contains stage logs/results, binary hashes,
tree IDs, the reconstructed tree, and focused runtime evidence when produced.
Build failures remain failures even if a later stage succeeds.

This checks intermediate compilation and the partitioned test wrapper. It does
not establish independent before/after bug proof for the read-buffer patch;
that evidence is still outstanding. No author sign-off is supplied here.

Three existing extra EOF blank lines from the validated candidate are retained
rather than silently changing the proven tree. Patch replay deliberately uses
--whitespace=nowarn to preserve those bytes. See tree-identity-proof.json and
patches.sha256 for exact identities.
