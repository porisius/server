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

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <regex>
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

// Writes through a temporary file in the same folder, so readers never see half a file.
auto writeAtomic(const std::filesystem::path& path, const std::string& contents) -> bool
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(contents.data(), static_cast<std::streamsize>(contents.size())))
        {
            return false;
        }
    }

    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        std::filesystem::remove(temporary, ec);
        return false;
    }

    return true;
}

auto readJsonFile(const std::filesystem::path& path) -> nlohmann::json
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return nullptr;
    }

    return nlohmann::json::parse(file, nullptr, false); // discarded (invalid) on a parse error
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

auto LauncherManifest::readState() -> nlohmann::json
{
    const auto statePath = std::filesystem::path(settings::get<std::string>("launcher.STATE_FILE"));
    if (statePath.empty())
    {
        return nlohmann::json::object();
    }

    std::scoped_lock lock(stateMutex_);
    auto state = readJsonFile(statePath);
    return state.is_object() ? state : nlohmann::json::object();
}

auto LauncherManifest::updateState(const nlohmann::json& patch) -> std::string
{
    if (!patch.is_object())
    {
        return "expected a JSON object";
    }

    for (const auto& [key, value] : patch.items())
    {
        const bool isBool   = key == "updateApproved" || key == "maintenance";
        const bool isString = key == "motd";
        if (!isBool && !isString)
        {
            return "unknown setting: " + key;
        }
        if (!value.is_null() && ((isBool && !value.is_boolean()) || (isString && !value.is_string())))
        {
            return key + (isBool ? " must be true, false or null" : " must be text or null");
        }
        if (isString && value.is_string() && value.get<std::string>().size() > 1000)
        {
            return "motd is longer than 1000 characters";
        }
    }

    const auto statePath = std::filesystem::path(settings::get<std::string>("launcher.STATE_FILE"));
    if (statePath.empty())
    {
        return "launcher.STATE_FILE is not set";
    }

    std::scoped_lock lock(stateMutex_);
    auto state = readJsonFile(statePath);
    if (!state.is_object())
    {
        state = nlohmann::json::object();
    }

    for (const auto& [key, value] : patch.items())
    {
        if (value.is_null())
        {
            state.erase(key);
        }
        else
        {
            state[key] = value;
        }
    }

    if (!writeAtomic(statePath, state.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n"))
    {
        return "cannot write " + statePath.generic_string();
    }

    return {};
}

auto LauncherManifest::effective() -> Effective
{
    const auto state = readState();

    const auto flag = [&](const char* key, const char* setting)
    {
        const auto it = state.find(key);
        return it != state.end() && it->is_boolean() ? it->get<bool>() : settings::get<bool>(setting);
    };

    Effective out{};
    out.updateApproved = flag("updateApproved", "launcher.CLIENT_UPDATE_APPROVED");
    // login.MAINT_MODE is the real lock (xi_connect enforces it), so it always shows
    out.maintenance = flag("maintenance", "launcher.MAINTENANCE") || settings::get<uint8>("login.MAINT_MODE") != 0;

    const auto motd = state.find("motd");
    out.motd        = motd != state.end() && motd->is_string() ? motd->get<std::string>() : settings::get<std::string>("launcher.MOTD");
    return out;
}

auto LauncherManifest::build() -> nlohmann::ordered_json
{
    const auto live = effective();

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
        { "updateApproved", live.updateApproved },
        // How login compares versions (first 6 characters): 0 off, 1 must match, 2 this or newer
        { "verLock", settings::get<uint8>("login.VER_LOCK") },
    };

    // xiloader: left out when it can't be hashed, so launchers fall back to their own copy
    // rather than being handed an entry they can't verify
    {
        const auto relative = settings::get<std::string>("launcher.XILOADER_FILE");
        const auto hash     = sha256Of(localPath(relative));

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

        if (hash.empty())
        {
            ShowErrorFmt("Launcher manifest: cannot read xiloader at {}; xiloader left out (is launcher.FILES_ROOT set?)",
                         localPath(relative).generic_string());
        }
        else
        {
            j["xiloader"] = {
                { "version", version },
                { "url", joinUrl(baseUrl, relative) },
                { "sha256", hash },
            };
        }
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

    j["maintenance"]  = live.maintenance;
    j["motd"]         = live.motd;
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

namespace
{

// Versions recorded by xiloader_update.py in a staging/ or archive/ folder (xiloader-<version>.json)
auto loaderVersionsIn(const std::filesystem::path& folder) -> std::vector<std::string>
{
    std::vector<std::string> versions;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
    {
        const auto name = entry.path().filename().string();
        if (name.starts_with("xiloader-") && entry.path().extension() == ".json")
        {
            if (const auto meta = readJsonFile(entry.path()); meta.is_object() && meta.value("version", "") != "")
            {
                versions.push_back(meta.value("version", ""));
            }
        }
    }

    std::sort(versions.begin(), versions.end(), std::greater<>());
    return versions;
}

const std::regex kLoaderVersion(R"(\d{1,4}\.\d{1,4}\.\d{1,4})");
const std::regex kClientVersion(R"(\d{8}_\d{1,4})");
const std::regex kSha256(R"([0-9A-Fa-f]{64})");

} // namespace

auto LauncherManifest::adminStatus(uint32_t playersOnline) -> nlohmann::ordered_json
{
    const auto filesRoot = std::filesystem::path(settings::get<std::string>("launcher.FILES_ROOT"));
    const auto clientVer = settings::get<std::string>("login.CLIENT_VER");
    const auto live      = effective();
    const auto state     = readState();

    nlohmann::ordered_json j;
    j["playersOnline"] = playersOnline;
    j["clientVersion"] = clientVer;
    j["lsbCommit"]     = version::GetGitSha();

    std::vector<std::string> overridden;
    for (const auto& [key, value] : state.items())
    {
        overridden.push_back(key);
    }

    j["gate"] = {
        { "updateApproved", live.updateApproved },
        { "maintenance", live.maintenance },
        { "loginMaintMode", settings::get<uint8>("login.MAINT_MODE") != 0 },
        { "motd", live.motd },
        { "overridden", overridden },
    };

    // xiloader: what's live, and what xiloader_update.py has staged or archived
    {
        const auto staging = filesRoot / "staging";

        nlohmann::ordered_json loader;
        const auto liveExe     = filesRoot / std::filesystem::path(settings::get<std::string>("launcher.XILOADER_FILE")).relative_path();
        const auto liveVersion = readTextFile(filesRoot / std::filesystem::path(settings::get<std::string>("launcher.XILOADER_VERSION_FILE")).relative_path());
        const auto liveHash    = sha256Of(liveExe);
        loader["live"]         = liveHash.empty() ? nlohmann::ordered_json(nullptr)
                                                  : nlohmann::ordered_json{ { "version", liveVersion }, { "sha256", liveHash } };
        loader["staged"]       = loaderVersionsIn(staging);
        loader["archived"]     = loaderVersionsIn(filesRoot / "archive");

        const auto held = readTextFile(staging / "hold");
        loader["held"]  = held.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(held);

        std::error_code ec;
        loader["requestPending"] = std::filesystem::exists(staging / "request.json", ec);

        const auto result    = readJsonFile(staging / "last-result.json");
        loader["lastResult"] = result.is_object() ? nlohmann::ordered_json(result) : nlohmann::ordered_json(nullptr);

        j["xiloader"] = loader;
    }

    // Verification manifests on hand
    {
        std::vector<std::string> versions;
        std::error_code          ec;
        for (const auto& entry : std::filesystem::directory_iterator(filesRoot / "verify", ec))
        {
            if (entry.path().extension() == ".json")
            {
                versions.push_back(entry.path().stem().string());
            }
        }
        std::sort(versions.begin(), versions.end(), std::greater<>());

        j["verify"] = {
            { "currentPresent", std::find(versions.begin(), versions.end(), clientVer) != versions.end() },
            { "versions", versions },
        };
    }

    return j;
}

auto LauncherManifest::requestXiloader(const std::string& action, const std::string& version) -> std::string
{
    if (action != "promote" && action != "rollback")
    {
        return "action must be promote or rollback";
    }
    if (!version.empty() && !std::regex_match(version, kLoaderVersion))
    {
        return "version must look like 3.0.1";
    }

    const auto staging = std::filesystem::path(settings::get<std::string>("launcher.FILES_ROOT")) / "staging";
    const auto request = staging / "request.json";

    std::scoped_lock lock(stateMutex_);

    std::error_code ec;
    if (std::filesystem::exists(request, ec))
    {
        return "another xiloader request is still waiting; is the xiloader-request.path unit running?";
    }

    const auto id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    nlohmann::ordered_json body = {
        { "id", std::to_string(id) },
        { "action", action },
        { "version", version.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(version) },
    };

    if (!writeAtomic(request, body.dump(2) + "\n"))
    {
        return "cannot write " + request.generic_string();
    }

    return {};
}

auto LauncherManifest::storeVerify(const std::string& clientVersion, const std::string& body) -> std::string
{
    if (!std::regex_match(clientVersion, kClientVersion))
    {
        return "the client version must look like 30261006_0";
    }

    const auto doc = nlohmann::json::parse(body, nullptr, false);
    if (!doc.is_object() || doc.value("schemaVersion", 0) != 1)
    {
        return "not a schemaVersion 1 verification manifest";
    }
    if (doc.value("clientVersion", "") != clientVersion)
    {
        return "its clientVersion doesn't match " + clientVersion;
    }

    const auto files = doc.find("files");
    if (files == doc.end() || !files->is_array() || files->empty())
    {
        return "it lists no files";
    }

    for (const auto& file : *files)
    {
        const auto path = file.is_object() ? file.value("path", "") : std::string();
        const auto hash = file.is_object() ? file.value("sha256", "") : std::string();
        if (path.empty() || path.front() == '/' || path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
            !std::regex_match(hash, kSha256) || !file.contains("size") || !file["size"].is_number_unsigned())
        {
            return "a file entry is malformed: " + path;
        }
    }

    const auto target = std::filesystem::path(settings::get<std::string>("launcher.FILES_ROOT")) / "verify" / (clientVersion + ".json");

    std::scoped_lock lock(stateMutex_);
    if (!writeAtomic(target, body))
    {
        return "cannot write " + target.generic_string();
    }

    ShowInfoFmt("Launcher admin: stored verification manifest {} ({} files)", target.generic_string(), files->size());
    return {};
}
