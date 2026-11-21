// Messages exchanged with the console.

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace core {

constexpr size_t kMaxBereiche = 255;
constexpr size_t kMaxDaten = 2048;

struct Bereich {
    uint32_t adresse = 0;
    uint16_t laenge = 0;
};

std::vector<uint8_t> AnfrageBereiche(uint8_t id, const std::vector<Bereich>& bereiche);
std::vector<uint8_t> AnfrageHash();

bool BereicheBilden(const std::set<uint32_t>& adressen, std::vector<Bereich>& ergebnis,
                    size_t max_bereiche = kMaxBereiche, size_t max_daten = kMaxDaten,
                    uint32_t luecke = 2);

struct DatenPaket {
    uint32_t bild = 0;
    uint8_t id = 0;
    bool aenderung = false;
    bool laeufe = false;
    std::vector<uint8_t> daten;
};

bool AenderungAnwenden(const DatenPaket& p, std::vector<uint8_t>& stand, size_t* laeufe_gezaehlt = nullptr,
                       size_t* bytes_gezaehlt = nullptr);

struct HashPaket {
    std::vector<std::string> hashes;
    std::string titel;
};

class Paketleser {
public:
    void Einlesen(const uint8_t* daten, size_t anzahl);
    bool NaechstesDatenpaket(DatenPaket& p);
    bool NaechstesHashpaket(HashPaket& p);
    uint64_t Fehler() const { return fehler_; }

    uint32_t MenueZeichen() { const uint32_t n = menue_; menue_ = 0; return n; }

    struct KabelPaket { uint8_t typ = 0; std::vector<uint8_t> daten; };
    bool NaechstesKabelpaket(KabelPaket& k) {
        if (kabel_.empty()) return false;
        k = kabel_.front();
        kabel_.erase(kabel_.begin());
        return true;
    }

    static constexpr size_t kDiagWorte = 10;
    bool NaechsteZaehler(uint32_t (&w)[kDiagWorte]) {
        if (!diag_neu_) return false;
        for (size_t i = 0; i < kDiagWorte; ++i) w[i] = diag_[i];
        diag_neu_ = false;
        return true;
    }

    struct GdPaket { uint8_t funktion = 0; uint32_t befehl = 0, ergebnis = 0, st0 = 0, st1 = 0; };
    bool NaechstesGdPaket(GdPaket& g) {
        if (gd_.empty()) return false;
        g = gd_.front();
        gd_.erase(gd_.begin());
        return true;
    }

    bool NaechsteBildrate(uint32_t (&w)[3]) {
        if (!bildrate_neu_) return false;
        for (int i = 0; i < 3; ++i) w[i] = bildrate_[i];
        bildrate_neu_ = false;
        return true;
    }

    uint32_t UeberschriebenMeldungen() { const uint32_t n = ueberschrieben_; ueberschrieben_ = 0; return n; }

private:
    uint32_t ueberschrieben_ = 0;
    uint32_t bildrate_[3] = {};
    bool bildrate_neu_ = false;
    std::vector<GdPaket> gd_;
    void Zerlegen();
    std::vector<uint8_t> puffer_;
    std::vector<DatenPaket> daten_;
    std::vector<HashPaket> hashes_;
    uint64_t fehler_ = 0;
    uint32_t menue_ = 0;
    uint32_t diag_[kDiagWorte] = {};
    std::vector<KabelPaket> kabel_;
    bool diag_neu_ = false;
};

}
