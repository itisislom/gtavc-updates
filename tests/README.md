# Tests Directory

This directory contains test suites for GTA Vice City Update Manager components.

## Testing Guidelines
- All destructive tests (install, replace, delete, rollback) MUST be executed exclusively within an isolated sandbox directory (`%TEMP%\gtavc_sandbox`).
- Real game installation directory (`D:\Games\Grand Theft Auto Vice City`) is strictly protected and never touched by automated tests.

## Test Suites
1. `path_tests.cpp` — Verifies `ResolveInsideRoot` validation rules against directory traversal, absolute paths, special devices, and reparse points.
2. `sandbox_tests.ps1` / `run_sandbox.bat` — Automated end-to-end integration tests (staging, transactions, rollback, SHA-256 validation).
