-----------------------------------
-- LAUNCHER MANIFEST SETTINGS
-----------------------------------
-- All settings are attached to the `xi.settings` object. This is published globally, and be accessed from C++ and any script.
--
-- This file controls the launcher manifest served by xi_world's HTTP server at
--   /api/launcher/manifest.json
-- It needs network.ENABLE_HTTP = true. Put a reverse proxy (nginx) in front of it;
-- leave network.HTTP_HOST on 'localhost'.
--
-- Live values are read on every request, so nothing here needs a server restart:
--   serverName       <- SERVER_NAME below, or main.SERVER_NAME when blank
--   client version   <- login.CLIENT_VER
--   maintenance      <- MAINTENANCE below, or login.MAINT_MODE ~= 0
--   ports            <- network.LOGIN_DATA_PORT / LOGIN_VIEW_PORT / LOGIN_AUTH_PORT / PROFILE_PORT
--   serverFingerprint <- the key in LOGIN_CERT_FILE (xiloader 3 format)
--   lsbCommit        <- the git commit xi_world was built from
--   sha256 values    <- hashed from the files under FILES_ROOT (re-hashed when a file changes)
--   launcher version <- the contents of LAUNCHER_VERSION_FILE
--   xiloader version <- the contents of XILOADER_VERSION_FILE, else XILOADER_VERSION
--
-- After editing this file (or login.lua), type `reload_settings` in the xi_world console.
-- Every setting can also be set with an environment variable, e.g. XI_LAUNCHER_MOTD.
-----------------------------------

xi = xi or {}
xi.settings = xi.settings or {}

xi.settings.launcher =
{
    -- Serve /api/launcher/manifest.json (read at startup: restart xi_world after changing it)
    ENABLED = false,

    -- Shown in the launcher. Leave blank to use main.SERVER_NAME.
    SERVER_NAME = '',

    -- Host name players' launchers connect to.
    SERVER_HOST = '',

    -- Public URL the files below are downloaded from, without a trailing slash.
    -- e.g. 'https://ffxi.example.com'
    PUBLIC_BASE_URL = '',

    -- Local folder nginx serves PUBLIC_BASE_URL from. File paths below are relative to it.
    -- e.g. '/var/www/lsb-launcher'
    FILES_ROOT = '',

    -- Client version check
    COMPARISON_MODE        = 'lsb-month',
    CLIENT_UPDATE_APPROVED = true,

    -- xiloader. The version shown comes from XILOADER_VERSION_FILE when that file exists
    -- (tools/launcher/xiloader_update.py writes it), otherwise from XILOADER_VERSION.
    XILOADER_VERSION      = '',
    XILOADER_VERSION_FILE = 'downloads/xiloader.version',
    XILOADER_FILE         = 'downloads/xiloader.exe',

    -- Client file verification. The manifest points at <VERIFY_DIR>/<login.CLIENT_VER>.json.
    -- If that file doesn't exist under FILES_ROOT, verification is advertised as disabled.
    VERIFY_DIR   = 'verify',
    QUICK_VERIFY = true,
    FULL_VERIFY  = true,

    -- Launcher self-update. The block is left out when either file is missing.
    LAUNCHER_FILE         = 'downloads/LSBLauncher.exe',
    LAUNCHER_VERSION_FILE = 'downloads/LSBLauncher.version',
    LAUNCHER_REQUIRED     = false,

    -- Forces maintenance on in the launcher even when login.MAINT_MODE is 0.
    MAINTENANCE = false,
    MOTD        = '',

    -- The login server's certificate (xi_connect creates it on first start, next to login.key).
    -- Its key fingerprint is published as serverFingerprint so launchers can pass it to
    -- xiloader 3 (--fingerprint): no trust prompt for players, and an impostor is refused.
    -- Relative paths are from the folder xi_world runs in. Back up login.key: a new key
    -- changes the fingerprint and every player's xiloader will refuse the server.
    LOGIN_CERT_FILE = 'login.cert',

    -- Admin API for Crystal's Settings -> Server tab (/api/launcher/admin/). Off while blank.
    -- Use 32+ random characters, e.g. from: openssl rand -base64 33
    -- Read at startup: restart xi_world after setting it the first time.
    ADMIN_TOKEN = '',

    -- Where the admin API keeps its overrides (update approval, maintenance, message).
    -- They take effect at once and win over the values in this file. Relative to xi_world's folder.
    STATE_FILE = 'launcher-state.json',

    -- Comma-separated list, e.g. '192.168.1.0/24,10.0.0.0/8'
    LAN_CIDRS      = '',
    HAIRPIN_ON_LAN = true,

    -- Optional direct FFXI updater (e.g. Kuluu). Left out of the manifest when disabled.
    DIRECT_UPDATER_ENABLED           = false,
    DIRECT_UPDATER_PROVIDER          = '',
    DIRECT_UPDATER_VERSION           = '',
    DIRECT_UPDATER_URL               = '',
    DIRECT_UPDATER_ARCHIVE_SHA256    = '',
    DIRECT_UPDATER_EXECUTABLE_SHA256 = '',
}
