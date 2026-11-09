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

#include "launcher_admin.h"
#include "launcher_manifest.h"

#include "common/logging.h"
#include "common/settings.h"

#include <openssl/crypto.h>

#include <chrono>
#include <string>
#include <thread>

namespace
{

constexpr std::size_t kMinimumTokenLength = 32;
constexpr std::size_t kMaximumVerifyBytes = 32 * 1024 * 1024;

auto currentToken() -> std::string
{
    return settings::get<std::string>("launcher.ADMIN_TOKEN");
}

// Constant-time comparison of the bearer token
auto authorized(const httplib::Request& req) -> bool
{
    const auto token = currentToken();
    if (token.size() < kMinimumTokenLength)
    {
        return false;
    }

    const auto  header = req.get_header_value("Authorization");
    const auto& prefix = std::string("Bearer ");
    if (header.size() != prefix.size() + token.size() || header.compare(0, prefix.size(), prefix) != 0)
    {
        return false;
    }

    return CRYPTO_memcmp(header.data() + prefix.size(), token.data(), token.size()) == 0;
}

void reply(httplib::Response& res, int status, const nlohmann::ordered_json& body)
{
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(body.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
}

void fail(httplib::Response& res, int status, const std::string& message)
{
    reply(res, status, nlohmann::ordered_json{ { "error", message } });
}

// Wraps a handler with the token check. A wrong token is slowed down and logged (without the token).
auto guarded(std::function<void(const httplib::Request&, httplib::Response&)> handler)
{
    return [handler = std::move(handler)](const httplib::Request& req, httplib::Response& res)
    {
        if (!authorized(req))
        {
            ShowWarningFmt("Launcher admin: refused {} {} from {}", req.method, req.path, req.get_header_value("X-Forwarded-For", req.remote_addr.c_str()));
            std::this_thread::sleep_for(std::chrono::seconds(1));
            fail(res, 401, "missing or wrong admin token");
            return;
        }

        handler(req, res);
    };
}

} // namespace

void registerLauncherAdminRoutes(httplib::Server& server, LauncherManifest& manifest, std::function<uint32_t()> playersOnline)
{
    if (currentToken().size() < kMinimumTokenLength)
    {
        if (!currentToken().empty())
        {
            ShowErrorFmt("Launcher admin: launcher.ADMIN_TOKEN is shorter than {} characters; the admin API is off", kMinimumTokenLength);
        }
        return;
    }

    ShowInfo("Launcher admin API enabled at /api/launcher/admin/");

    server.Get("/api/launcher/admin/status", guarded([&manifest, playersOnline](const httplib::Request&, httplib::Response& res)
                                                     { reply(res, 200, manifest.adminStatus(playersOnline())); }));

    server.Put("/api/launcher/admin/state", guarded([&manifest, playersOnline](const httplib::Request& req, httplib::Response& res)
                                                    {
                                                        const auto patch = nlohmann::json::parse(req.body, nullptr, false);
                                                        if (const auto error = manifest.updateState(patch); !error.empty())
                                                        {
                                                            fail(res, 400, error);
                                                            return;
                                                        }
                                                        ShowInfoFmt("Launcher admin: state changed: {}", patch.dump());
                                                        reply(res, 200, manifest.adminStatus(playersOnline()));
                                                    }));

    server.Post("/api/launcher/admin/xiloader", guarded([&manifest](const httplib::Request& req, httplib::Response& res)
                                                        {
                                                            const auto body    = nlohmann::json::parse(req.body, nullptr, false);
                                                            const auto action  = body.is_object() ? body.value("action", "") : std::string();
                                                            const auto version = body.is_object() && body.contains("version") && body["version"].is_string()
                                                                                     ? body["version"].get<std::string>()
                                                                                     : std::string();
                                                            if (const auto error = manifest.requestXiloader(action, version); !error.empty())
                                                            {
                                                                fail(res, error.starts_with("another") ? 409 : 400, error);
                                                                return;
                                                            }
                                                            ShowInfoFmt("Launcher admin: xiloader {} {} requested", action, version.empty() ? "(newest)" : version);
                                                            reply(res, 202, nlohmann::ordered_json{ { "queued", true } });
                                                        }));

    server.Put(R"(/api/launcher/admin/verify/([0-9_]+))", guarded([&manifest](const httplib::Request& req, httplib::Response& res)
                                                                  {
                                                                      if (req.body.size() > kMaximumVerifyBytes)
                                                                      {
                                                                          fail(res, 413, "the verification manifest is larger than 32 MB");
                                                                          return;
                                                                      }
                                                                      if (const auto error = manifest.storeVerify(req.matches[1].str(), req.body); !error.empty())
                                                                      {
                                                                          fail(res, 400, error);
                                                                          return;
                                                                      }
                                                                      reply(res, 200, nlohmann::ordered_json{ { "stored", req.matches[1].str() } });
                                                                  }));
}
