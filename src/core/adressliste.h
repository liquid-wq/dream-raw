// Memory ranges an achievement set needs.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/dcprotokoll.h"
#include "core/ra.h"

namespace core {

constexpr uint8_t kVorgabeListe = 0xFE;

std::vector<Bereich> AdresslisteAusSpiel(const SpielDaten& s, size_t& adressen);

bool AdresslisteSpeichern(uint32_t spiel_id, const std::vector<Bereich>& b);
bool AdresslisteLaden(uint32_t spiel_id, std::vector<Bereich>& b);

std::vector<uint8_t> AdressenBinBauen(const std::vector<std::pair<int, std::vector<Bereich>>>& listen);

}
