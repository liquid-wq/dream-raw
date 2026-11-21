// Session: cable, game detection and achievement evaluation.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace core {

struct RamZeile {
    uint32_t adresse = 0;
    uint8_t wert = 0;
    uint8_t vorher = 0;
    bool empfangen = false;
};

struct SitzungsMeldung {

    enum class Art { Log, Warnung, Fehler, Gut, Verbindung, Port, Spiel, Freigeschaltet, Statistik, Ram, Kennung };
    bool unbekannt = false;

    bool diag_ok = false;
    uint32_t diag_w0 = 0, diag_w1 = 0, diag_w2 = 0, diag_w3 = 0, diag_w4 = 0;
    uint32_t diag_w5 = 0, diag_w6 = 0, diag_w7 = 0, diag_w8 = 0, diag_w9 = 0;
    Art art = Art::Log;
    std::string text;

    bool kabel_offen = false;
    bool daten_kommen = false;
    bool daten_aktuell = false;
    bool im_menue = false;

    std::string hash;
    std::string titel;
    uint32_t spiel_id = 0;

    uint32_t achievement_id = 0;
    uint32_t punkte = 0;
    std::string beschreibung;
    std::string badge;
    std::vector<uint32_t> bereits;

    uint32_t bilder_pro_s = 0;
    float spiel_bilder_s = -1;
    float rechenanteil = -1;
    uint32_t bereiche = 0;
    uint32_t bytes_pro_bild = 0;
    uint64_t pruefsummenfehler = 0;
    uint64_t bytes_empfangen = 0;
    std::vector<RamZeile> ram;
    uint32_t adressen_gesamt = 0;
};

class Sitzung {
public:
    using Melder = std::function<void(const SitzungsMeldung&)>;

    explicit Sitzung(Melder melder);
    ~Sitzung();

    void Starten(const std::string& port, const std::string& benutzer, const std::string& token);
    void Beenden();
    bool Laeuft() const { return laeuft_; }

private:
    void Schleife(std::string port, std::string benutzer, std::string token);

    Melder melder_;
    std::thread thread_;
    std::atomic<bool> stopp_{false};
    std::atomic<bool> laeuft_{false};
    std::atomic<bool> abzug_neu_{false};
    uint32_t abzug_adresse_ = 0, abzug_laenge_ = 0;
    std::wstring abzug_datei_;
};

}
