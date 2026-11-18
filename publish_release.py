"""
Automated release builder and publisher for GTA Vice City updates.
Usage:
    python publish_release.py --version 1.1.0 --message "Release notes here"
"""
import os
import sys
import json
import hashlib
import zipfile
import argparse
import subprocess
import urllib.request
import urllib.error

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
UPDATER_DIR = os.path.join(ROOT_DIR, "_tools", "gtavc-updates")
DIST_DIR = os.path.join(UPDATER_DIR, "dist")
STAGING_DIR = os.path.join(DIST_DIR, "staging")
BUILD_DIR = os.path.join(UPDATER_DIR, "build")
REPO_URL = "https://api.github.com/repos/itisislom/gtavc-updates"


def get_git_token():
    p = subprocess.run(
        ["git", "credential", "fill"],
        cwd=UPDATER_DIR,
        input=b"protocol=https\nhost=github.com\n\n",
        stdout=subprocess.PIPE,
        check=True
    )
    creds = dict(line.split("=", 1) for line in p.stdout.decode().splitlines() if "=" in line)
    token = creds.get("password")
    if not token:
        raise RuntimeError("GitHub token could not be retrieved from git credential helper.")
    return token


def build_binaries():
    print("[1/6] Building UpdateChecker.asi and updater.exe...")
    bat = os.path.join(UPDATER_DIR, "build.bat")
    subprocess.run([bat], cwd=UPDATER_DIR, check=True)

    for bin_name in ["UpdateChecker.asi", "updater.exe"]:
        src = os.path.join(BUILD_DIR, bin_name)
        if not os.path.exists(src):
            raise FileNotFoundError(f"Build output missing: {src}")
        shutil_copy(src, os.path.join(STAGING_DIR, bin_name))
        shutil_copy(src, os.path.join(ROOT_DIR, bin_name))
        alt_root = r"D:\Grand Theft Auto Vice City"
        if os.path.exists(alt_root):
            shutil_copy(src, os.path.join(alt_root, bin_name))
    print("  Binaries built and synced successfully.")


def shutil_copy(src, dst):
    import shutil
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def create_zip(zip_path, staging_dir):
    print("[2/6] Packaging update.zip...")
    if os.path.exists(zip_path):
        os.remove(zip_path)

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for root, _, files in os.walk(staging_dir):
            for file in files:
                abs_p = os.path.join(root, file)
                rel_p = os.path.relpath(abs_p, staging_dir)
                zf.write(abs_p, rel_p)

    sz_mb = os.path.getsize(zip_path) / (1024 * 1024)
    print(f"  Package created: {sz_mb:.2f} MB")


def compute_sha256(file_path):
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()


def write_clean_sha256_file(sha_path, hash_hex):
    # CRITICAL: Always pure ASCII without BOM, 64 hex chars + newline (65 bytes)
    with open(sha_path, "w", encoding="ascii", newline="\n") as f:
        f.write(hash_hex + "\n")
    sz = os.path.getsize(sha_path)
    if sz != 65:
        raise ValueError(f"Invalid sha256 file size: {sz} bytes (expected 65 bytes ASCII).")
    print(f"  Clean ASCII sha256 written ({sz} bytes).")


def update_manifest(version, hash_hex, message):
    print("[3/6] Updating version.json...")
    manifest_p = os.path.join(UPDATER_DIR, "version.json")
    data = {
        "version": version,
        "download_url": f"https://github.com/itisislom/gtavc-updates/releases/download/v{version}/update.zip",
        "message": message,
        "mandatory": True,
        "sha256": hash_hex
    }
    with open(manifest_p, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")

    # Update version.txt in staging and game root
    for vpath in [os.path.join(STAGING_DIR, "version.txt"), os.path.join(ROOT_DIR, "version.txt")]:
        with open(vpath, "w", encoding="ascii", newline="\n") as f:
            f.write(f"{version}\n")
    print("  Manifest and version files updated.")


def git_commit_and_push(version):
    print("[4/6] Pushing version.json and source changes to GitHub...")
    subprocess.run(["git", "add", "version.json", "src/updater/main.cpp"], cwd=UPDATER_DIR, check=True)
    subprocess.run(["git", "commit", "-m", f"Release v{version}"], cwd=UPDATER_DIR, check=False)
    subprocess.run(["git", "push", "origin", "main"], cwd=UPDATER_DIR, check=True)
    print("  Git push complete.")


def create_github_release(token, version, message, zip_path, sha_path):
    print(f"[5/6] Creating GitHub Release v{version}...")
    headers = {
        "Authorization": f"token {token}",
        "Content-Type": "application/json",
        "User-Agent": "gtavc-publisher"
    }

    # Delete existing release with same tag if exists
    try:
        req = urllib.request.Request(f"{REPO_URL}/releases/tags/v{version}", headers=headers)
        with urllib.request.urlopen(req) as resp:
            old_rel = json.loads(resp.read().decode())
            old_id = old_rel.get("id")
            if old_id:
                print(f"  Deleting prior release {old_id} with tag v{version}...")
                del_req = urllib.request.Request(f"{REPO_URL}/releases/{old_id}", headers=headers, method="DELETE")
                urllib.request.urlopen(del_req)
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise

    # Create new release
    rel_body = {
        "tag_name": f"v{version}",
        "target_commitish": "main",
        "name": f"Release v{version}: {message}",
        "body": f"Update v{version}:\n- {message}",
        "draft": False,
        "prerelease": False
    }
    req = urllib.request.Request(f"{REPO_URL}/releases", data=json.dumps(rel_body).encode("utf-8"), headers=headers)
    with urllib.request.urlopen(req) as resp:
        rel = json.loads(resp.read().decode())

    upload_url = rel.get("upload_url").split("{")[0]
    print(f"  Release created. Uploading assets to {upload_url}...")

    # Upload update.zip
    with open(zip_path, "rb") as f:
        zip_bytes = f.read()
    up_req = urllib.request.Request(
        f"{upload_url}?name=update.zip",
        data=zip_bytes,
        headers={"Authorization": f"token {token}", "Content-Type": "application/zip", "User-Agent": "gtavc-publisher"}
    )
    with urllib.request.urlopen(up_req) as resp:
        print("  update.zip uploaded (HTTP", resp.status, ")")

    # Upload update.zip.sha256 (strictly pure ASCII)
    with open(sha_path, "rb") as f:
        sha_bytes = f.read()
    up_sha_req = urllib.request.Request(
        f"{upload_url}?name=update.zip.sha256",
        data=sha_bytes,
        headers={"Authorization": f"token {token}", "Content-Type": "text/plain", "User-Agent": "gtavc-publisher"}
    )
    with urllib.request.urlopen(up_sha_req) as resp:
        print("  update.zip.sha256 uploaded (HTTP", resp.status, ")")


def verify_release(version, expected_hash):
    print("[6/6] Verifying release integrity...")
    dl_url = f"https://github.com/itisislom/gtavc-updates/releases/download/v{version}/update.zip"
    sha_url = f"https://github.com/itisislom/gtavc-updates/releases/download/v{version}/update.zip.sha256"

    req_sha = urllib.request.Request(sha_url, headers={"User-Agent": "gtavc-check"})
    with urllib.request.urlopen(req_sha) as resp:
        raw_sha = resp.read().decode("ascii").strip()

    if raw_sha.lower() != expected_hash.lower():
        raise ValueError(f"Integrity check failed! Remote sha256 {raw_sha} != expected {expected_hash}")

    print("  Remote SHA-256 matches expected:", raw_sha)
    print(f"SUCCESS: Release v{version} published and verified.")


def main():
    parser = argparse.ArgumentParser(description="Publish GTA VC update release")
    parser.add_argument("--version", required=True, help="Semantic version, e.g. 1.1.0")
    parser.add_argument("--message", required=True, help="Release description")
    parser.add_argument("--skip-build", action="store_true", help="Skip binary compilation")
    args = parser.parse_args()

    version = args.version.lstrip("v")
    os.makedirs(DIST_DIR, exist_ok=True)
    zip_path = os.path.join(DIST_DIR, "update.zip")
    sha_path = os.path.join(DIST_DIR, "update.zip.sha256")

    token = get_git_token()

    if not args.skip_build:
        build_binaries()

    create_zip(zip_path, STAGING_DIR)
    h = compute_sha256(zip_path)
    print("  SHA-256:", h)

    write_clean_sha256_file(sha_path, h)
    update_manifest(version, h, args.message)
    git_commit_and_push(version)
    create_github_release(token, version, args.message, zip_path, sha_path)
    verify_release(version, h)


if __name__ == "__main__":
    main()
