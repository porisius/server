# Launcher manifest

xi_world can serve the launcher's `manifest.json` itself, so there is no generator
script or systemd unit to keep in sync. Every request builds the manifest from:

| Manifest field | Source |
|---|---|
| `serverName` | `launcher.SERVER_NAME`, or `main.SERVER_NAME` when blank |
| `serverHost` | `launcher.SERVER_HOST` |
| `ports` | `network.LOGIN_DATA_PORT`, `LOGIN_VIEW_PORT`, `LOGIN_AUTH_PORT`, `PROFILE_PORT` |
| `client.requiredVersion` | `login.CLIENT_VER` |
| `xiloader.sha256` | hashed from `FILES_ROOT/XILOADER_FILE` |
| `verification` | `PUBLIC_BASE_URL/VERIFY_DIR/<CLIENT_VER>.json`; disabled when that file is missing |
| `maintenance` | `launcher.MAINTENANCE` or `login.MAINT_MODE ~= 0` |
| `launcher` | version from `LAUNCHER_VERSION_FILE`, SHA-256 of `LAUNCHER_FILE`; omitted if either is missing |
| `lsbCommit` | the git commit xi_world was built from |
| `serverFingerprint` | the key in `LOGIN_CERT_FILE` (`login.cert`), in xiloader 3's `SHA256:...` form |

Hashes are cached per file and recomputed when its size or modification time changes.

Launchers that read `serverFingerprint` pass it to xiloader 3 as `--fingerprint`: players are never
asked to trust the server, and a server presenting a different key is refused. That makes
`login.key` precious: back it up. If it's lost or regenerated, the fingerprint changes and players
who pinned the old one are refused until their launcher reads the new manifest.

## Setup

1. Copy `settings/default/launcher.lua` to `settings/launcher.lua` and fill it in:

   ```lua
   xi.settings.launcher =
   {
       ENABLED          = true,
       SERVER_NAME      = 'My Home LSB',
       SERVER_HOST      = 'ffxi.example.com',
       PUBLIC_BASE_URL  = 'https://ffxi.example.com',
       FILES_ROOT       = '/var/www/lsb-launcher',
       XILOADER_VERSION = '2.2.0',
       LAN_CIDRS        = '192.168.2.0/24',
   }
   ```

   Keys you leave out keep their defaults.

2. In `settings/network.lua`, set `ENABLE_HTTP = true`. Keep `HTTP_HOST = 'localhost'`.
3. Rebuild and restart xi_world. It logs
   `Serving launcher manifest on http://localhost:8088/api/launcher/manifest.json`.
4. Install `nginx-launcher.conf.example` as a site. It maps the public `/manifest.json` to that
   route, so existing launchers keep working with no change.
5. Check: `curl -s https://ffxi.example.com/manifest.json`

## Day to day

| You did | What to do |
|---|---|
| Changed `login.CLIENT_VER`, `MAINT_MODE`, or anything in `launcher.lua` | Type `reload_settings` in the xi_world console |
| Uploaded a new `xiloader.exe` | Nothing |
| Published a new launcher (`LSBLauncher.exe` + `LSBLauncher.version`) | Nothing |
| Generated a verify manifest for a new client version | Put it in `verify/<CLIENT_VER>.json`; nothing else |
| Pulled and rebuilt LSB | Restart xi_world; `lsbCommit` updates |

Upload new files under a temporary name and `mv` them into place, so a request never
hashes a half-copied EXE. (One that does is re-hashed on the next request rather than cached.)

`login.CLIENT_VER` and `MAINT_MODE` are read separately by xi_connect and xi_world. After changing
them, reload or restart both so the login check and the manifest agree.

## xiloader updates

`xiloader_update.py` checks [LandSandBoat/xiloader](https://github.com/LandSandBoat/xiloader)
releases, downloads a newer `xiloader.exe` to `FILES_ROOT/staging/` and verifies it against
the hash the release publishes. Players only get it once it's promoted into `downloads/`,
which also writes `downloads/xiloader.version` (the label in the manifest). The previous
release is kept in `archive/` for rollback.

```
python3 tools/launcher/xiloader_update.py status
python3 tools/launcher/xiloader_update.py check      # exit 10 = staged, waiting for you
python3 tools/launcher/xiloader_update.py promote    # publish the newest staged release
python3 tools/launcher/xiloader_update.py rollback   # put the previous one back
```

After a rollback, `check` skips the version you rolled back from (and older), so the nightly
timer won't put it back. The next newer release is staged as usual.

Before the first promote, give the current EXE its version so it can be archived:

```
echo 2.2.0 > /srv/lsb-launcher/downloads/xiloader.version
```

`--auto-promote patch` publishes 3.0.0 -> 3.0.1 without asking, `minor` also 3.0 -> 3.1.
A new major version always waits for `promote`: read its release notes and test one
launch first.

Daily check with systemd (adjust the path in the service if your checkout isn't `/home/lsb/server`):

```
sudo cp tools/launcher/systemd/xiloader-update.{service,timer} /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now xiloader-update.timer
journalctl -u xiloader-update     # what the last checks found
```
