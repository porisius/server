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

#include "launcher_manifest.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/version.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <array>
#include <fstream>
#include <memory>
#include <string_view>
#include <vector>

namespace
{

auto trim(std::string_view text) -> std::string
{
    constexpr std::string_view whitespace = " \t\r\n";

    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos)
    {
        return {};
    }

    const auto last = text.find_last_not_of(whitespace);
    return std::string(text.substr(first, last - first + 1));
}

auto splitList(const std::string& text) -> std::vector<std::string>
{
    std::vector<std::string> out;

    std::size_t start = 0;
    while (start <= text.size())
    {
        auto end = text.find(',', start);
        if (end == std::string::npos)
        {
            end = text.size();
        }

        if (auto item = trim(std::string_view(text).substr(start, end - start)); !item.empty())
        {
            out.emplace_back(std::move(item));
        }

        start = end + 1;
    }

    return out;
}

// Joins a base URL and a relative path with exactly one slash between them.
auto joinUrl(std::string base, std::string_view path) -> std::string
{
    while (!base.empty() && base.back() == '/')
    {
        base.pop_back();
    }

    while (!path.empty() && path.front() == '/')
    {
        path.remove_prefix(1);
    }

    return base + "/" + std::string(path);
}

auto readTextFile(const std::filesystem::path& path) -> std::string
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return {};
    }

    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return trim(contents);
}

auto hashFile(const std::filesystem::path& path) -> std::string
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return {};
    }

    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
    {
        return {};
    }

    std::vector<char> buffer(1 << 20);
    while (file)
    {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (const auto count = file.gcount(); count > 0)
        {
            if (EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
            {
                return {};
            }
        }
    }

    if (file.bad())
    {
        return {};
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int                               digestLength = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digestLength) != 1)
    {
        return {};
    }

    constexpr std::string_view hexDigits = "0123456789ABCDEF";

    std::string hex;
    hex.reserve(digestLength * 2);
    for (unsigned int i = 0; i < digestLength; ++i)
    {
        hex.push_back(hexDigits[digest[i] >> 4]);
        hex.push_back(hexDigits[digest[i] & 0x0F]);
    }

    return hex;
}

// What xiloader 3 shows and checks: the SHA-256 of the certificate's SubjectPublicKeyInfo (DER),
// base64 without padding, after "SHA256:". The same as:
//   openssl x509 -in login.cert -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | base64
auto certificateFingerprint(const std::filesystem::path& certPath) -> std::string
{
    const std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_file(certPath.string().c_str(), "r"), &BIO_free);
    if (!bio)
    {
        return {};
    }

    // The first certificate in the file is the one the login server presents
    const std::unique_ptr<X509, decltype(&X509_free)> cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
    if (!cert)
    {
        return {};
    }

    unsigned char* der    = nullptr;
    const int      length = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert.get()), &der);
    if (length <= 0 || der == nullptr)
    {
        return {};
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int                               digestLength = 0;
    const bool hashed = EVP_Digest(der, static_cast<std::size_t>(length), digest.data(), &digestLength, EVP_sha256(), nullptr) == 1;
    OPENSSL_free(der);
    if (!hashed)
    {
        return {};
    }

    std::array<unsigned char, 4 * ((EVP_MAX_MD_SIZE + 2) / 3) + 1> encoded{};
    const int encodedLength = EVP_EncodeBlock(encoded.data(), digest.data(), static_cast<int>(digestLength));
    if (encodedLength <= 0)
    {
        return {};
    }

    std::string text(reinterpret_cast<const char*>(encoded.data()), static_cast<std::size_t>(encodedLength));
    while (!text.empty() && text.back() == '=')
    {
        text.pop_back();
    }

    return "SHA256:" + text;
}

} // namespace

auto LauncherManifest::fingerprintOf(const std::filesystem::path& certPath) -> std::string
{
    std::error_code ec;

    const auto mtime = std::filesystem::last_write_time(certPath, ec);
    const auto size  = ec ? 0 : std::filesystem::file_size(certPath, ec);
    if (ec)
    {
        return {};
    }

    const auto key = "fingerprint:" + certPath.generic_string();

    {
        std::scoped_lock lock(hashMutex_);
        if (const auto it = hashCache_.find(key); it != hashCache_.end() && it->second.mtime == mtime && it->second.size == size)
        {
            return it->second.hex;
        }
    }

    auto fingerprint = certificateFingerprint(certPath);
    if (!fingerprint.empty())
    {
        std::scoped_lock lock(hashMutex_);
        hashCache_[key] = CachedHash{ mtime, size, fingerprint };
    }

    return fingerprint;
}

auto LauncherManifest::sha256Of(const std::filesystem::path& path) -> std::string
{
    std::error_code ec;

    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        return {};
    }

    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        return {};
    }

    const auto key = path.generic_string();

    {
        std::scoped_lock lock(hashMutex_);
        if (const auto it = hashCache_.find(key); it != hashCache_.end() && it->second.mtime == mtime && it->second.size == size)
        {
            return it->second.hex;
        }
    }

    // Hash outside the lock: a large file shouldn't hold up requests for the others.
    auto hex = hashFile(path);
    if (hex.empty())
    {
        return {};
    }

    // A file that changed while it was hashed (an upload still being copied in, say)
    // is reported but not cached, so the next request hashes it again.
    const auto mtimeAfter = std::filesystem::last_write_time(path, ec);
    const auto sizeAfter  = ec ? 0 : std::filesystem::file_size(path, ec);
    if (!ec && mtimeAfter == mtime && sizeAfter == size)
    {
        std::scoped_lock lock(hashMutex_);
        hashCache_[key] = CachedHash{ mtime, size, hex };
    }

    return hex;
}

auto LauncherManifest::build() -> nlohmann::ordered_json
{
    const auto filesRoot = std::filesystem::path(settings::get<std::string>("launcher.FILES_ROOT"));
    const auto baseUrl   = settings::get<std::string>("launcher.PUBLIC_BASE_URL");
    const auto clientVer = settings::get<std::string>("login.CLIENT_VER");

    const auto localPath = [&](const std::string& relative)
    {
        return filesRoot / std::filesystem::path(relative).relative_path();
    };

    auto serverName = settings::get<std::string>("launcher.SERVER_NAME");
    if (serverName.empty())
    {
        serverName = settings::get<std::string>("main.SERVER_NAME");
    }

    nlohmann::ordered_json j;

    j["schemaVersion"] = 1;
    j["serverName"]    = serverName;
    j["serverHost"]    = settings::get<std::string>("launcher.SERVER_HOST");

    j["ports"] = {
        { "data", settings::get<uint16>("network.LOGIN_DATA_PORT") },
        { "view", settings::get<uint16>("network.LOGIN_VIEW_PORT") },
        { "auth", settings::get<uint16>("network.LOGIN_AUTH_PORT") },
        { "profile", settings::get<uint16>("network.PROFILE_PORT") },
    };

    j["client"] = {
        { "requiredVersion", clientVer },
        { "comparisonMode", settings::get<std::string>("launcher.COMPARISON_MODE") },
        { "updateApproved", settings::get<bool>("launcher.CLIENT_UPDATE_APPROVED") },
    };

    // xiloader
    {
        const auto relative = settings::get<std::string>("launcher.XILOADER_FILE");
        const auto hash     = sha256Of(localPath(relative));
        if (hash.empty())
        {
            ShowErrorFmt("Launcher manifest: cannot read xiloader at {}", localPath(relative).generic_string());
        }

        // The version file (written by tools/launcher/xiloader_update.py) wins over the setting
        std::string version;
        if (const auto versionFile = settings::get<std::string>("launcher.XILOADER_VERSION_FILE"); !versionFile.empty())
        {
            version = readTextFile(localPath(versionFile));
        }

        if (version.empty())
        {
            version = settings::get<std::string>("launcher.XILOADER_VERSION");
        }

        j["xiloader"] = {
            { "version", version },
            { "url", joinUrl(baseUrl, relative) },
            { "sha256", hash },
        };
    }

    // Client verification, keyed by the client version the server expects
    {
        auto relative = joinUrl(settings::get<std::string>("launcher.VERIFY_DIR"), clientVer + ".json");
        if (relative.front() == '/') // VERIFY_DIR left blank
        {
            relative.erase(0, 1);
        }

        std::error_code ec;
        const bool      exists = std::filesystem::is_regular_file(localPath(relative), ec);
        if (!exists)
        {
            ShowWarningFmt("Launcher manifest: no verification manifest for {} at {}; verification disabled",
                           clientVer,
                           localPath(relative).generic_string());
        }

        j["verification"] = {
            { "url", joinUrl(baseUrl, relative) },
            { "quickEnabled", exists && settings::get<bool>("launcher.QUICK_VERIFY") },
            { "fullEnabled", exists && settings::get<bool>("launcher.FULL_VERIFY") },
        };
    }

    if (settings::get<bool>("launcher.DIRECT_UPDATER_ENABLED"))
    {
        j["directUpdater"] = {
            { "enabled", true },
            { "provider", settings::get<std::string>("launcher.DIRECT_UPDATER_PROVIDER") },
            { "version", settings::get<std::string>("launcher.DIRECT_UPDATER_VERSION") },
            { "url", settings::get<std::string>("launcher.DIRECT_UPDATER_URL") },
            { "archiveSha256", settings::get<std::string>("launcher.DIRECT_UPDATER_ARCHIVE_SHA256") },
            { "executableSha256", settings::get<std::string>("launcher.DIRECT_UPDATER_EXECUTABLE_SHA256") },
        };
    }

    j["maintenance"]  = settings::get<bool>("launcher.MAINTENANCE") || settings::get<uint8>("login.MAINT_MODE") != 0;
    j["motd"]         = settings::get<std::string>("launcher.MOTD");
    j["lanCidrs"]     = splitList(settings::get<std::string>("launcher.LAN_CIDRS"));
    j["hairpinOnLan"] = settings::get<bool>("launcher.HAIRPIN_ON_LAN");

    // Launcher self-update: only advertised when both the EXE and its version file exist
    {
        const auto exeRelative = settings::get<std::string>("launcher.LAUNCHER_FILE");
        const auto version     = readTextFile(localPath(settings::get<std::string>("launcher.LAUNCHER_VERSION_FILE")));
        const auto hash        = version.empty() ? std::string() : sha256Of(localPath(exeRelative));

        if (!version.empty() && !hash.empty())
        {
            j["launcher"] = {
                { "version", version },
                { "url", joinUrl(baseUrl, exeRelative) },
                { "sha256", hash },
                { "required", settings::get<bool>("launcher.LAUNCHER_REQUIRED") },
            };
        }
    }

    // Lets launchers pass xiloader 3's --fingerprint, so players aren't asked to trust the server
    // and a server presenting another key is refused
    {
        const auto certPath    = std::filesystem::path(settings::get<std::string>("launcher.LOGIN_CERT_FILE"));
        const auto fingerprint = certPath.empty() ? std::string() : fingerprintOf(certPath);
        if (fingerprint.empty())
        {
            ShowWarningFmt("Launcher manifest: cannot read the login certificate at {}; serverFingerprint left out",
                           certPath.generic_string());
        }
        else
        {
            j["serverFingerprint"] = fingerprint;
        }
    }

    j["lsbCommit"] = version::GetGitSha();

    return j;
}
