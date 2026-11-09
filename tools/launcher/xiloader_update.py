#!/usr/bin/env python3
"""Stage, verify and publish xiloader releases for the launcher manifest.

New releases of https://github.com/LandSandBoat/xiloader are downloaded to a staging
folder and verified there. Nothing reaches players until a release is promoted into
downloads/, either by hand or, if you allow it, automatically for patch or minor releases.
A major version is never promoted automatically.

xi_world re-hashes downloads/xiloader.exe and reads downloads/xiloader.version on the next
manifest request, so promoting is the whole update.

    xiloader_update.py check                      # stage a newer release, if there is one
    xiloader_update.py check --auto-promote patch # ...and publish it if only the patch number changed
    xiloader_update.py status                     # what's live, staged and archived
    xiloader_update.py promote [VERSION]          # publish a staged release (newest if omitted)
    xiloader_update.py rollback [VERSION]         # put an archived release back (newest if omitted)
    xiloader_update.py handle-request             # carry out staging/request.json (from the admin API)

Layout under --root (default /srv/lsb-launcher):
    downloads/xiloader.exe, downloads/xiloader.version   served to players
    staging/                                             verified, not yet published
    archive/                                             releases replaced by a promote

Only staging/ and archive/ are written outside downloads/; the nginx example serves
nothing but downloads/ and verify/.

Exit codes: 0 nothing to do or done, 1 error, 10 a new release is staged and waiting.
"""

from __future__ import annotations

import argparse
import datetime
import fcntl
import hashlib
import json
import os
import re
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_ROOT = "/srv/lsb-launcher"
DEFAULT_API = "https://api.github.com/repos/LandSandBoat/xiloader/releases/latest"
ASSET_NAME = "xiloader.exe"
USER_AGENT = "lsb-xiloader-updater"
MAX_ASSET_BYTES = 64 * 1024 * 1024

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_STAGED = 10

Version = tuple[int, int, int]


class UpdateError(Exception):
    pass


def log(message: str) -> None:
    print(message, flush=True)


def parse_version(text: str) -> Version | None:
    match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", text.strip())
    return tuple(int(part) for part in match.groups()) if match else None  # type: ignore[return-value]


def version_text(version: Version) -> str:
    return ".".join(str(part) for part in version)


def file_hash(path: Path, algorithm: str) -> str:
    digest = hashlib.new(algorithm)
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


class Layout:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.downloads = root / "downloads"
        self.staging = root / "staging"
        self.archive = root / "archive"
        self.live_exe = self.downloads / ASSET_NAME
        self.live_version = self.downloads / "xiloader.version"
        self.hold_file = self.staging / "hold"

    def prepare(self) -> None:
        if not self.downloads.is_dir():
            raise UpdateError(f"{self.downloads} does not exist; is --root right?")
        self.staging.mkdir(mode=0o750, exist_ok=True)
        self.archive.mkdir(mode=0o750, exist_ok=True)

    def live(self) -> Version | None:
        try:
            return parse_version(self.live_version.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return None

    @staticmethod
    def _entries(folder: Path) -> dict[Version, tuple[Path, dict]]:
        found: dict[Version, tuple[Path, dict]] = {}
        for meta_path in folder.glob("xiloader-*.json"):
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                continue
            version = parse_version(meta.get("version", ""))
            exe = meta_path.with_suffix(".exe")
            if version and exe.is_file():
                found[version] = (exe, meta)
        return found

    def held(self) -> Version | None:
        """Releases up to this version were rolled back from; check leaves them alone."""
        try:
            return parse_version(self.hold_file.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return None

    def staged(self) -> dict[Version, tuple[Path, dict]]:
        return self._entries(self.staging)

    def archived(self) -> dict[Version, tuple[Path, dict]]:
        return self._entries(self.archive)


def write_atomic(target: Path, data: bytes, scratch_dir: Path) -> None:
    """Write via a temp file on the same file system, so readers see the old or the new file, never half of one."""
    fd, temp_name = tempfile.mkstemp(dir=scratch_dir, prefix=".tmp-")
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temp_name, 0o644)
        os.replace(temp_name, target)
    except BaseException:
        Path(temp_name).unlink(missing_ok=True)
        raise


def copy_atomic(source: Path, target: Path, scratch_dir: Path) -> None:
    write_atomic(target, source.read_bytes(), scratch_dir)


def http_get(url: str, accept: str) -> urllib.request.addinfourl:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": accept})
    token = os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        request.add_header("Authorization", f"Bearer {token}")
    try:
        return urllib.request.urlopen(request, timeout=60)
    except urllib.error.HTTPError as error:
        raise UpdateError(f"GET {url} failed: HTTP {error.code}") from error
    except urllib.error.URLError as error:
        raise UpdateError(f"GET {url} failed: {error.reason}") from error


def fetch_latest_release(api_url: str) -> dict:
    with http_get(api_url, "application/vnd.github+json") as response:
        return json.load(response)


def expected_hashes(release: dict, asset: dict) -> dict[str, str]:
    """Hashes to check the download against: GitHub's asset digest and/or the MD5 in the release notes."""
    hashes: dict[str, str] = {}
    digest = asset.get("digest") or ""
    if digest.lower().startswith("sha256:"):
        hashes["sha256"] = digest.split(":", 1)[1].lower()
    match = re.search(r"MD5:\s*([0-9A-Fa-f]{32})", release.get("body") or "")
    if match:
        hashes["md5"] = match.group(1).lower()
    return hashes


def download_and_verify(release: dict, asset: dict, layout: Layout, allow_unverified: bool) -> Path:
    expected = expected_hashes(release, asset)
    if not expected and not allow_unverified:
        raise UpdateError(
            "the release publishes no SHA-256 digest or MD5 to check the download against; "
            "re-run with --allow-unverified if you accept that"
        )

    size = int(asset.get("size") or 0)
    if size <= 0 or size > MAX_ASSET_BYTES:
        raise UpdateError(f"unexpected asset size {size}")

    fd, temp_name = tempfile.mkstemp(dir=layout.staging, prefix=".download-")
    temp = Path(temp_name)
    try:
        with os.fdopen(fd, "wb") as handle, http_get(asset["browser_download_url"], "application/octet-stream") as response:
            received = 0
            for chunk in iter(lambda: response.read(1 << 20), b""):
                received += len(chunk)
                if received > MAX_ASSET_BYTES:
                    raise UpdateError("download is larger than the release says")
                handle.write(chunk)

        if received != size:
            raise UpdateError(f"download is {received} bytes, release says {size}")

        with temp.open("rb") as handle:
            if handle.read(2) != b"MZ":
                raise UpdateError("download is not a Windows executable")

        for algorithm, want in expected.items():
            got = file_hash(temp, algorithm)
            if got != want:
                raise UpdateError(f"{algorithm.upper()} mismatch: release says {want}, download is {got}")
            log(f"  {algorithm.upper()} verified")

        return temp
    except BaseException:
        temp.unlink(missing_ok=True)
        raise


def promote(layout: Layout, version: Version | None, source: str = "staging") -> Version:
    entries = layout.staged() if source == "staging" else layout.archived()
    if not entries:
        raise UpdateError(f"nothing in {source}/")

    if version is None:
        version = max(entries)
    if version not in entries:
        available = ", ".join(version_text(v) for v in sorted(entries)) or "none"
        raise UpdateError(f"{version_text(version)} is not in {source}/ (available: {available})")

    exe, meta = entries[version]
    if file_hash(exe, "sha256") != meta.get("sha256"):
        raise UpdateError(f"{exe} no longer matches the hash recorded when it was staged; refusing to publish it")

    # Keep what is live now, so it can be rolled back to
    live_version = layout.live()
    if live_version is None and layout.live_exe.is_file():
        raise UpdateError(
            f"{layout.live_exe} has no xiloader.version next to it, so it can't be archived for rollback. "
            f"Write its version first, e.g.: echo 2.2.0 > {layout.live_version}"
        )
    if live_version and layout.live_exe.is_file() and live_version != version:
        archive_exe = layout.archive / f"xiloader-{version_text(live_version)}.exe"
        if not archive_exe.exists():
            copy_atomic(layout.live_exe, archive_exe, layout.archive)
            archive_meta = {"version": version_text(live_version), "sha256": file_hash(archive_exe, "sha256")}
            write_atomic(archive_exe.with_suffix(".json"), json.dumps(archive_meta, indent=2).encode(), layout.archive)

    # EXE first, then its version label. xi_world reads both on every manifest request.
    copy_atomic(exe, layout.live_exe, layout.staging)
    write_atomic(layout.live_version, f"{version_text(version)}\n".encode(), layout.staging)

    if source == "staging":
        exe.unlink()
        exe.with_suffix(".json").unlink()

    # Publishing anything by hand lifts a hold from an earlier rollback (rollback sets a new one)
    layout.hold_file.unlink(missing_ok=True)

    log(f"published xiloader {version_text(version)} ({meta['sha256'].upper()})")
    return version


def auto_promote_allowed(policy: str, live: Version | None, new: Version) -> bool:
    if live is None or policy == "none":
        return False
    if policy == "patch":
        return new[:2] == live[:2]
    if policy == "minor":
        return new[0] == live[0]
    return False


def command_check(args: argparse.Namespace, layout: Layout) -> int:
    live = layout.live()
    release = fetch_latest_release(args.api_url)

    tag = release.get("tag_name", "")
    version = parse_version(tag)
    if version is None:
        raise UpdateError(f"latest release tag {tag!r} isn't a version number")
    if release.get("draft") or release.get("prerelease"):
        log(f"latest release {tag} is a draft or prerelease; ignoring it")
        return EXIT_OK

    if live and version <= live:
        log(f"up to date: xiloader {version_text(live)} is live, latest release is {tag}")
        return EXIT_OK

    held = layout.held()
    if held and version <= held:
        log(f"holding: you rolled back from {version_text(held)}, so {tag} is skipped. "
            f"A newer release will be staged as usual; to publish {version_text(version)} anyway: "
            f"{Path(sys.argv[0]).name} rollback {version_text(version)}")
        return EXIT_OK

    staged = layout.staged()
    if version not in staged:
        asset = next((a for a in release.get("assets", []) if a.get("name") == ASSET_NAME), None)
        if asset is None:
            raise UpdateError(f"release {tag} has no {ASSET_NAME}")

        log(f"downloading xiloader {tag}")
        temp = download_and_verify(release, asset, layout, args.allow_unverified)

        exe = layout.staging / f"xiloader-{version_text(version)}.exe"
        os.chmod(temp, 0o644)
        os.replace(temp, exe)
        meta = {
            "version": version_text(version),
            "tag": tag,
            "sha256": file_hash(exe, "sha256"),
            "published_at": release.get("published_at"),
            "release_url": release.get("html_url"),
        }
        write_atomic(exe.with_suffix(".json"), json.dumps(meta, indent=2).encode(), layout.staging)
        log(f"staged xiloader {version_text(version)} at {exe}")
    else:
        log(f"xiloader {version_text(version)} is already staged")

    if auto_promote_allowed(args.auto_promote, live, version):
        promote(layout, version)
        return EXIT_OK

    notes = staged.get(version, (None, {}))[1].get("release_url") or release.get("html_url") or ""
    why = "first install" if live is None else (
        "major version" if version[0] != live[0] else f"--auto-promote is {args.auto_promote}")
    log(f"waiting for approval ({why}). Release notes: {notes}")
    log(f"publish with: {Path(sys.argv[0]).name} promote {version_text(version)}")
    return EXIT_STAGED


def command_status(_args: argparse.Namespace, layout: Layout) -> int:
    live = layout.live()
    if live and layout.live_exe.is_file():
        log(f"live:     {version_text(live)}  sha256 {file_hash(layout.live_exe, 'sha256').upper()}")
    elif layout.live_exe.is_file():
        log("live:     xiloader.exe present, but no xiloader.version")
        log(f"          write its version so it can be archived: echo 2.2.0 > {layout.live_version}")
    else:
        log("live:     none")

    for label, entries in (("staged", layout.staged()), ("archived", layout.archived())):
        listed = ", ".join(version_text(v) for v in sorted(entries, reverse=True)) or "none"
        log(f"{label + ':':9} {listed}")
    if held := layout.held():
        log(f"held:     {version_text(held)} and older (from a rollback)")
    return EXIT_OK


def requested_version(text: str | None) -> Version | None:
    if text is None:
        return None
    version = parse_version(text)
    if version is None:
        raise UpdateError(f"{text!r} isn't a version number like 3.0.1")
    return version


def command_promote(args: argparse.Namespace, layout: Layout) -> int:
    promote(layout, requested_version(args.version), "staging")
    return EXIT_OK


def command_rollback(args: argparse.Namespace, layout: Layout) -> int:
    archived = layout.archived()
    live = layout.live()
    if args.version:
        target = requested_version(args.version)
    else:
        older = [v for v in archived if live is None or v < live]
        if not older:
            raise UpdateError("no archived release older than the live one")
        target = max(older)

    rolled_back_from = live
    promote(layout, target, "archive")
    if rolled_back_from and target < rolled_back_from:
        layout.hold_file.write_text(f"{version_text(rolled_back_from)}\n", encoding="utf-8")
        log(f"holding back {version_text(rolled_back_from)} and older; check will skip them")
    return EXIT_OK


def command_handle_request(_args: argparse.Namespace, layout: Layout) -> int:
    """Carries out staging/request.json, written by xi_world's admin API, into staging/last-result.json."""
    request_path = layout.staging / "request.json"
    try:
        request = json.loads(request_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return EXIT_OK
    except (OSError, json.JSONDecodeError):
        request = {}
    # Removed first, so the path unit doesn't run again for the same request
    request_path.unlink(missing_ok=True)

    action = request.get("action") if isinstance(request, dict) else None
    version = request.get("version") if isinstance(request, dict) else None
    result = {"id": request.get("id") if isinstance(request, dict) else None, "action": action, "version": version}
    try:
        if action == "promote":
            done = promote(layout, requested_version(version), "staging")
            result["message"] = f"published xiloader {version_text(done)}"
        elif action == "rollback":
            before = layout.live()
            command_rollback(argparse.Namespace(version=version), layout)
            result["message"] = f"rolled back from {version_text(before) if before else 'none'} to {version_text(layout.live())}"
        else:
            raise UpdateError(f"unknown request {action!r}")
        result["ok"] = True
    except UpdateError as error:
        result["ok"] = False
        result["message"] = str(error)
        log(f"error: {error}")

    result["finishedAt"] = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    write_atomic(layout.staging / "last-result.json", json.dumps(result, indent=2).encode(), layout.staging)
    return EXIT_OK if result["ok"] else EXIT_ERROR


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path(os.environ.get("XILOADER_UPDATE_ROOT", DEFAULT_ROOT)),
                        help=f"launcher files folder, i.e. launcher.FILES_ROOT (default {DEFAULT_ROOT})")
    commands = parser.add_subparsers(dest="command", required=True)

    check = commands.add_parser("check", help="stage the latest release if it is newer than the live one")
    check.add_argument("--auto-promote", choices=("none", "patch", "minor"), default="none",
                       help="publish without approval when only the patch (or minor) number changed; never for a major")
    check.add_argument("--allow-unverified", action="store_true",
                       help="stage a release even if it publishes no hash to check the download against")
    check.add_argument("--api-url", default=DEFAULT_API, help=argparse.SUPPRESS)
    check.set_defaults(handler=command_check)

    commands.add_parser("status", help="show the live, staged and archived versions").set_defaults(handler=command_status)

    promote_parser = commands.add_parser("promote", help="publish a staged release")
    promote_parser.add_argument("version", nargs="?", help="version to publish (default: newest staged)")
    promote_parser.set_defaults(handler=command_promote)

    rollback = commands.add_parser("rollback", help="put an archived release back; check then skips the version rolled back from")
    rollback.add_argument("version", nargs="?", help="version to restore (default: newest older than live)")
    rollback.set_defaults(handler=command_rollback)

    commands.add_parser("handle-request", help="carry out a promote/rollback asked for through the admin API").set_defaults(
        handler=command_handle_request)

    args = parser.parse_args()
    layout = Layout(args.root)

    try:
        layout.prepare()
        with (layout.staging / ".lock").open("w") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise UpdateError("another run is in progress")
            return args.handler(args, layout)
    except UpdateError as error:
        log(f"error: {error}")
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
