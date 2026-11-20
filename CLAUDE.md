# wine-rta

A fork of Wine patched for real-time audio (plugins run through yabridge).
`master` mirrors upstream Wine; the work happens on feature branches.

Before doing anything, read `rta/HANDOFF.md`, then
`rta/design/01-realtime-scheduling.md`. Everything specific to this fork is
under `rta/` (design notes, references, benchmarks, tools).

- New behaviour is opt-in through `WINE_RT_PRIO=1`; with it off, Linux scheduling
  must stay as in upstream Wine.
- Keep patches small, flag-gated and separable for easy rebasing onto upstream.
- Commit messages follow Wine's style (`component: Summary.`); fork documentation
  uses the `rta:` prefix and goes in separate commits from code.
