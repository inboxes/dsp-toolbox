## What
<!-- One or two sentences. For contributor tasks: "Closes #<issue>". -->

## Acceptance checklist
<!-- Paste the issue's acceptance list and tick each item. -->

## Evidence
<!-- Test summary (cases/assertions), key measured numbers, micro-benchmark results if the issue has a budget. -->

## Deviations from the spec
<!-- None, or what and why (agreed in issue #...). -->

## Checks
- [ ] Only paths in the issue's Scope are changed
- [ ] `dsp-common.yml` green on Ubuntu and Windows (contributor tasks) / `ci.yml` green
- [ ] Code and data are GPLv3-compatible (no AGPL, GPLv2-only or proprietary code); sources cited in comments
- [ ] No allocation in per-block calls (tests use `AllocGuard`)
