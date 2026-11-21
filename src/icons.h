// Game icons for the console menu and the start logo.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/gdiabbild.h"

namespace icons {

std::vector<core::GdiDatei> IconsFuerMenue(const std::vector<core::KartenPlatz>& plaetze, const std::string& benutzer,
                                           const std::string& token, std::vector<std::string>& meldungen,
                                           const std::function<void(const std::string&)>& fortschritt = nullptr);

std::vector<uint8_t> StartlogoMr();

}
