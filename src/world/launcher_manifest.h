/*
===========================================================================

  Copyright (c) 2026 LandSandBoat Dev Teams

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses/

===========================================================================
*/

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

//
// Builds the launcher manifest (manifest.json) from settings/launcher.lua, the live
// login/main/network settings, the build's git commit and the files under launcher.FILES_ROOT.
//
// build() is called from httplib's worker threads; it only reads settings and the file system.
//
class LauncherManifest
{
public:
    auto build() -> nlohmann::ordered_json;

    // ---- used by the admin API (launcher_admin.cpp) ----

    // Overrides set from the admin API, kept in launcher.STATE_FILE: "updateApproved",
    // "maintenance" (bools) and "motd" (string). Missing keys fall back to settings/launcher.lua.
    auto readState() -> nlohmann::json;

    // Merges `patch` into the state file; a null value removes that override. Returns an error
    // message, or "" on success.
    auto updateState(const nlohmann::json& patch) -> std::string;

    // Everything the admin screen shows.
    auto adminStatus(uint32_t playersOnline) -> nlohmann::ordered_json;

    // Asks tools/launcher/xiloader_update.py (run by its systemd path unit) to promote or roll back.
    // `version` may be empty for "newest". Returns an error message, or "" on success.
    auto requestXiloader(const std::string& action, const std::string& version) -> std::string;

    // Stores verify/<clientVersion>.json after checking it looks like a verification manifest.
    // Returns an error message, or "" on success.
    auto storeVerify(const std::string& clientVersion, const std::string& body) -> std::string;

private:
    struct Effective
    {
        bool        updateApproved;
        bool        maintenance;
        std::string motd;
    };

    // Settings with the admin overrides applied
    auto effective() -> Effective;

    // Upper-case hex SHA-256 of a file, or "" if it can't be read. Cached until the file's
    // modification time or size changes.
    auto sha256Of(const std::filesystem::path& path) -> std::string;

    // The login server's key fingerprint in xiloader's format ("SHA256:" + unpadded base64
    // of the SHA-256 of the certificate's public key), or "" if the certificate can't be read.
    // Cached the same way as sha256Of.
    auto fingerprintOf(const std::filesystem::path& certPath) -> std::string;

    struct CachedHash
    {
        std::filesystem::file_time_type mtime;
        std::uintmax_t                  size;
        std::string                     hex;
    };

    std::mutex                                  hashMutex_;
    std::unordered_map<std::string, CachedHash> hashCache_;

    std::mutex stateMutex_; // serializes state file and request file writes
};
