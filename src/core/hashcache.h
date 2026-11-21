// Local cache of game identifications.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "core/ra.h"

namespace core {

class HashCache {
public:

    static HashCache& Global();

    HashAufloesung Aufloesen(const std::string& hash, bool unbekannt_neu_pruefen = false);

    bool Nachsehen(const std::string& hash, uint32_t& spiel_id);

    void Eintragen(const std::map<std::string, uint32_t>& neu);

private:
    void Laden();
    void Speichern() const;
    std::wstring pfad_;
    bool geladen_ = false;
    std::map<std::string, uint32_t> eintraege_;
    std::mutex sperre_;
};

}
