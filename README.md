# GTA: Vice City Update Repository

This repository hosts the update manifest (`version.json`) and release assets for GTA: Vice City updates by Islom Sadriddinov.

## How to release an update:
1. Update `"version"` and `"download_url"` in `version.json`.
2. Commit and push changes to `main`.
3. Create a GitHub Release with the tag matching `"version"` (e.g. `v1.0.1`) and attach `update.zip`.
