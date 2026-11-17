# GTA: Vice City - In-Game Update Manager

Created by **Islom Sadriddinov**

An in-game update management system for GTA: Vice City with GUI dialogs, GitHub Releases integration, freedom of choice (no forced updates), and automated rollback support.

---

## Features

- **In-Game Version Manager:** Press `0` (or Numpad `0`) at any time during gameplay to open the Update Manager.
- **Freedom of Choice:** Lists the 5 most recent GitHub releases. You can install an update, reinstall the current version, or roll back to an older version. Or simply click **Play / Close** to continue playing.
- **Automatic Clean Rollback:** When downgrading/rolling back to an older version, any files that were introduced in newer releases are automatically and cleanly removed, keeping the game directory clean.
- **One-Time Intro:** First-launch dialog introduces the player to the update system once, guarded by `updater.ini`.
- **Zero Dependencies:** Pure Win32 API and native Windows HTTP/WinINet stack without external runtimes.

---

## Quick Installation

Copy the following files into your GTA Vice City game root directory (where `gta-vc.exe` is located):
1. `updater.exe`
2. `UpdateChecker.asi` (and optionally `scripts/UpdateChecker.asi`)
3. `version.txt`

Launch `gta-vc.exe`. During gameplay, press `0` to manage updates.

---

## Repository Structure

- `updater.exe` — The standalone updater application that downloads, extracts, and manages file rollbacks.
- `UpdateChecker.asi` — The ASI plugin that runs in GTA: Vice City, listens for key `0`, and displays the Update Manager UI.
- `version.txt` — Base local version file.
- `version.json` — Manifest metadata for fallback version checks.
- `src/checker/` — C++ source code for `UpdateChecker.asi`.
- `src/updater/` — C++ source code for `updater.exe`.

---

## How to Publish New Updates

1. Create a GitHub Release with a tag matching your version format (e.g. `v1.0.4`).
2. Attach `update.zip` containing your updated/new files (maintaining game folder paths like `scripts/`, `modloader/`, etc.).
3. If specific files must be deleted upon updating, include a `delete.txt` in the root of `update.zip` listing the relative paths to remove.
4. Update `version.json` on branch `main` with the new version and release download URL.
