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
#include <functional>

#include <httplib.h>

class LauncherManifest;

//
// The launcher admin API, for Crystal's Settings → Server tab. Every route needs
//   Authorization: Bearer <launcher.ADMIN_TOKEN>
// and is only registered when that token is set (32 characters or more).
//
//   GET  /api/launcher/admin/status                 overview (players, gate, xiloader, verify files)
//   PUT  /api/launcher/admin/state                  {"updateApproved","maintenance","motd"}; null clears
//   POST /api/launcher/admin/xiloader               {"action":"promote"|"rollback","version":"3.0.1"?}
//   PUT  /api/launcher/admin/verify/<clientVersion> a verification manifest (verify/<version>.json)
//
void registerLauncherAdminRoutes(httplib::Server& server, LauncherManifest& manifest, std::function<uint32_t()> playersOnline);
