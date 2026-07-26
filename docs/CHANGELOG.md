# Changelog

This file is updated by the release helper in `scripts/release.ps1`.

## v1.0.22 - 2026-07-26

- Removed dead LoRa macro definitions from `src/main.c`.
- Merged radio access into a single `radio_task` owner path.
- Routed time sync, control, collision-check, and relay sends through the queued TX path.
- Added constant-time PIN matching for web and duress login handling.
- Added release automation scripts for version bumps, changelog generation, and tag creation.
- Added a GitHub Actions release workflow.

## Unreleased

- No release has been cut since the last version bump.
