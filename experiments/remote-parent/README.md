# Remote-parent design experiments

The experiment branches share this test harness and start from commit
`379355f58041f2a6008ef61c33869c65551610ee`.

Each branch contains:

- `route`, naming the workflow under test;
- `route.patch`, applied by the experiment workflow before building;
- the same read/splice, multi-round, wire-byte, image-layout, and restore checks.

The patches are experimental. The selected design will be rebuilt as normal,
reviewable commits before the upstream pull request is updated.
