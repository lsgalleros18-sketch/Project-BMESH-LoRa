# Changelog

This file is updated by the release helper in `scripts/release.ps1`.

## v1.0.23 - 2026-08-04

- Moved the portal HTML from firmware string literals into LittleFS under `data/`.
- Added LittleFS mounting and file streaming in `src/main.c` so the web portal is served from `/littlefs`.
- Reworked the 16MB partition table to keep a single factory app and dedicate the remaining flash to LittleFS storage and coredump space.
- Updated PlatformIO to use the custom partition table and LittleFS filesystem image.
- Documented the LittleFS workflow so portal asset changes require `pio run -e esp32-s3-devkitm-1 -t buildfs -t uploadfs`.
- Kept OTA deferred and explicitly not implemented.

## v1.0.22 - 2026-07-26

- Removed dead LoRa macro definitions from `src/main.c`.
- Merged radio access into a single `radio_task` owner path.
- Routed time sync, control, collision-check, and relay sends through the queued TX path.
- Added constant-time PIN matching for web and duress login handling.
- Added release automation scripts for version bumps, changelog generation, and tag creation.
- Added a GitHub Actions release workflow.

## Unreleased

- No release has been cut since the last version bump.

//
pio run -e esp32-s3-devkitm-1 -t buildfs
pio run -e esp32-s3-devkitm-1 -t uploadfs
//
