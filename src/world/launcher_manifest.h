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

private:
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
};
