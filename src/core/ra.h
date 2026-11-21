// RetroAchievements requests.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace core {

constexpr uint32_t kKonsoleDreamcast = 40;

struct Anmeldung {
    bool ok = false;
    std::string fehler;
    std::string benutzer;
    std::string token;
    uint32_t punkte = 0;
    uint32_t punkte_softcore = 0;
};

Anmeldung Anmelden(const std::string& benutzer, const std::string& passwort);

Anmeldung AnmeldenMitToken(const std::string& benutzer, const std::string& token);

struct HashErgebnis {
    bool ok = false;
    std::string hash;
    std::string fehler;
    std::string meldungen;
};

HashErgebnis ImageHashen(const std::string& pfad_utf8);

struct IpFelder {
    bool ok = false;
    std::string geraet;
    std::string bereich;
    std::string peripherie;
};

HashErgebnis ImageHashenMitPatch(const std::string& pfad_utf8, const char* bereich8, int vga,
                                 IpFelder* original);

struct HashAufloesung {
    bool ok = false;
    std::string fehler;
    uint32_t spiel_id = 0;
};

HashAufloesung HashAufloesen(const std::string& hash);

struct HashBibliothek {
    bool ok = false;
    std::string fehler;
    std::map<std::string, uint32_t> hashes;
};
HashBibliothek HashBibliothekLaden(uint32_t konsole);

struct Achievement {
    uint32_t id = 0;
    uint32_t punkte = 0;
    bool offiziell = true;
    std::string titel;
    std::string beschreibung;
    std::string definition;
};

struct SpielDaten {
    bool ok = false;
    std::string fehler;
    uint32_t id = 0;
    uint32_t konsole = 0;
    std::string titel;
    std::string icon_url;
    std::vector<Achievement> achievements;
    std::vector<std::pair<uint32_t, std::string>> leaderboards;
    std::string rich_presence;
};

SpielDaten SpielLaden(const std::string& benutzer, const std::string& token, uint32_t spiel_id);

}
