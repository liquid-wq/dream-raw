// Messages exchanged with the console.

#include "core/dcprotokoll.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace core {
namespace {

void Summe8Anhaengen(std::vector<uint8_t>& v) {
    uint8_t s = 0;
    for (uint8_t b : v) s = static_cast<uint8_t>(s + b);
    v.push_back(s);
}

}

std::vector<uint8_t> AnfrageBereiche(uint8_t id, const std::vector<Bereich>& bereiche) {
    std::vector<uint8_t> v = {'R', 'Q', id, static_cast<uint8_t>(bereiche.size())};
    for (const Bereich& b : bereiche) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(b.adresse >> (8 * i)));
        v.push_back(static_cast<uint8_t>(b.laenge));
        v.push_back(static_cast<uint8_t>(b.laenge >> 8));
    }
    Summe8Anhaengen(v);
    return v;
}

std::vector<uint8_t> AnfrageHash() {
    std::vector<uint8_t> v = {'R', 'H'};
    Summe8Anhaengen(v);
    return v;
}

bool BereicheBilden(const std::set<uint32_t>& adressen, std::vector<Bereich>& ergebnis,
                    size_t max_bereiche, size_t max_daten, uint32_t luecke) {
    ergebnis.clear();
    struct Spanne { uint32_t von, bis; };
    std::vector<Spanne> s;
    for (uint32_t a : adressen) {
        if (a > 0xFFFFFF) continue;
        if (!s.empty() && a <= s.back().bis + luecke) s.back().bis = std::max(s.back().bis, a + 1);
        else s.push_back({a, a + 1});
    }

    while (s.size() > max_bereiche) {
        size_t beste = 0;
        uint32_t kleinste = UINT32_MAX;
        for (size_t i = 0; i + 1 < s.size(); ++i) {
            const uint32_t l = s[i + 1].von - s[i].bis;
            if (l < kleinste) { kleinste = l; beste = i; }
        }
        s[beste].bis = s[beste + 1].bis;
        s.erase(s.begin() + static_cast<long>(beste) + 1);
    }
    size_t summe = 0;
    for (const Spanne& sp : s) {
        size_t len = sp.bis - sp.von;
        if (len > 0xFFFF) len = 0xFFFF;
        if (summe + len > max_daten) {
            const size_t rest = max_daten - summe;
            if (rest) ergebnis.push_back({sp.von, static_cast<uint16_t>(rest)});
            return false;
        }
        ergebnis.push_back({sp.von, static_cast<uint16_t>(len)});
        summe += len;
    }
    return true;
}

void Paketleser::Einlesen(const uint8_t* daten, size_t anzahl) {
    puffer_.insert(puffer_.end(), daten, daten + anzahl);
    Zerlegen();
    if (puffer_.size() > (1u << 20)) puffer_.clear();
}

void Paketleser::Zerlegen() {
    size_t pos = 0;
    while (puffer_.size() - pos >= 4) {
        const uint8_t* p = puffer_.data() + pos;

        if (p[0] == 'D' && p[1] == 'C' && p[2] == 'R' && p[3] == 'W') { ++ueberschrieben_; pos += 4; continue; }
        if (puffer_.size() - pos < 5) break;
        if (!(p[0] == 'D' && p[1] == 'C' && p[2] == 'R' &&
              (p[3] == 'D' || p[3] == 'H' || p[3] == 'A' || p[3] == 'L' || p[3] == 'M' || p[3] == 'X' || p[3] == 'K' || p[3] == 'G' || p[3] == 'B'))) { ++pos; continue; }
        const size_t rest = puffer_.size() - pos;
        if (p[3] == 'B') {
            const size_t gesamt = 4 + 12 + 1;
            if (rest < gesamt) break;
            uint8_t s = 0;
            for (size_t i = 0; i < gesamt - 1; ++i) s = static_cast<uint8_t>(s + p[i]);
            if (s != p[gesamt - 1]) { ++fehler_; ++pos; continue; }
            for (int i = 0; i < 3; ++i)
                bildrate_[i] = p[4 + i * 4] | (p[5 + i * 4] << 8) | (p[6 + i * 4] << 16) | (static_cast<uint32_t>(p[7 + i * 4]) << 24);
            bildrate_neu_ = true;
            pos += gesamt;
            continue;
        }
        if (p[3] == 'G') {
            const size_t gesamt = 22;
            if (rest < gesamt) break;
            uint8_t s = 0;
            for (size_t i = 0; i < gesamt - 1; ++i) s = static_cast<uint8_t>(s + p[i]);
            if (s != p[gesamt - 1]) { ++fehler_; ++pos; continue; }
            GdPaket g;
            g.funktion = p[4];
            uint32_t w[4];
            for (int i = 0; i < 4; ++i)
                w[i] = p[5 + i * 4] | (p[6 + i * 4] << 8) | (p[7 + i * 4] << 16) | (static_cast<uint32_t>(p[8 + i * 4]) << 24);
            g.befehl = w[0]; g.ergebnis = w[1]; g.st0 = w[2]; g.st1 = w[3];
            gd_.push_back(g);
            pos += gesamt;
            continue;
        }
        if (p[3] == 'K') {
            if (rest < 6) break;
            const size_t gesamt = 6 + p[5] + 1;
            if (rest < gesamt) break;
            uint8_t s = 0;
            for (size_t i = 0; i < gesamt - 1; ++i) s = static_cast<uint8_t>(s + p[i]);
            if (s != p[gesamt - 1]) { ++fehler_; ++pos; continue; }
            KabelPaket k;
            k.typ = p[4];
            k.daten.assign(p + 6, p + 6 + p[5]);
            kabel_.push_back(std::move(k));
            pos += gesamt;
            continue;
        }
        if (p[3] == 'X') {
            const size_t gesamt = 4 + kDiagWorte * 4 + 1;
            if (rest < gesamt) break;
            uint8_t s = 0;
            for (size_t i = 0; i < gesamt - 1; ++i) s = static_cast<uint8_t>(s + p[i]);
            if (s != p[gesamt - 1]) { ++fehler_; ++pos; continue; }
            for (size_t i = 0; i < kDiagWorte; ++i)
                diag_[i] = p[4 + i * 4] | (p[5 + i * 4] << 8) | (p[6 + i * 4] << 16) | (static_cast<uint32_t>(p[7 + i * 4]) << 24);
            diag_neu_ = true;
            pos += gesamt;
            continue;
        }
        if (p[3] == 'M') {
            if (rest < 6) break;
            uint8_t s = 0;
            for (size_t i = 0; i < 5; ++i) s = static_cast<uint8_t>(s + p[i]);
            if (s != p[5]) { ++fehler_; ++pos; continue; }
            ++menue_;
            pos += 6;
            continue;
        }
        if (p[3] == 'H') {
            const size_t n = p[4];
            if (n == 0 || n > 32) { ++pos; continue; }
            const size_t gesamt = 5 + n * 32 + 64 + 1;
            if (rest < gesamt) break;
            uint8_t s = 0;
            for (size_t i = 0; i < gesamt - 1; ++i) s = static_cast<uint8_t>(s + p[i]);
            if (s != p[gesamt - 1]) { ++fehler_; ++pos; continue; }
            HashPaket h;
            for (size_t i = 0; i < n; ++i) h.hashes.emplace_back(reinterpret_cast<const char*>(p + 5 + i * 32), 32);
            const char* t = reinterpret_cast<const char*>(p + 5 + n * 32);
            h.titel.assign(t, strnlen(t, 64));
            hashes_.push_back(h);
            pos += gesamt;
            continue;
        }
        if (rest < 11) break;

        const bool laeufe = p[3] == 'L';
        const bool aenderung = p[3] == 'A' || laeufe;
        const size_t feld = p[9] | (p[10] << 8);
        const size_t len = p[3] == 'A' ? feld * 3 : feld;
        if (len > kMaxDaten) { ++pos; continue; }
        if (rest < 11 + len + 2) break;
        uint16_t s = 0;
        for (size_t i = 0; i < len; ++i) s = static_cast<uint16_t>(s + p[11 + i]);
        const uint16_t soll = static_cast<uint16_t>(p[11 + len] | (p[12 + len] << 8));
        if (s != soll) { ++fehler_; ++pos; continue; }
        DatenPaket d;
        d.bild = p[4] | (p[5] << 8) | (p[6] << 16) | (static_cast<uint32_t>(p[7]) << 24);
        d.id = p[8];
        d.aenderung = aenderung;
        d.laeufe = laeufe;
        d.daten.assign(p + 11, p + 11 + len);
        daten_.push_back(std::move(d));
        pos += 11 + len + 2;
    }
    puffer_.erase(puffer_.begin(), puffer_.begin() + static_cast<long>(pos));
}

bool AenderungAnwenden(const DatenPaket& p, std::vector<uint8_t>& stand, size_t* laeufe_gezaehlt, size_t* bytes_gezaehlt) {
    size_t laeufe = 0, bytes = 0;
    if (!p.laeufe) {
        size_t vorher = SIZE_MAX;
        for (size_t i = 0; i + 2 < p.daten.size(); i += 3) {
            const size_t index = p.daten[i] | (p.daten[i + 1] << 8);
            if (index < stand.size()) stand[index] = p.daten[i + 2];
            if (index != vorher + 1) ++laeufe;
            vorher = index;
            ++bytes;
        }
    } else {
        size_t pos = 0, i = 0;
        while (i < p.daten.size()) {
            if (i + 1 >= p.daten.size()) return false;
            pos += p.daten[i];
            const size_t n = p.daten[i + 1];
            i += 2;
            if (i + n > p.daten.size()) return false;
            for (size_t k = 0; k < n; ++k, ++pos)
                if (pos < stand.size()) stand[pos] = p.daten[i + k];
            i += n;
            if (n) { ++laeufe; bytes += n; }
        }
    }
    if (laeufe_gezaehlt) *laeufe_gezaehlt = laeufe;
    if (bytes_gezaehlt) *bytes_gezaehlt = bytes;
    return true;
}

bool Paketleser::NaechstesDatenpaket(DatenPaket& p) {
    if (daten_.empty()) return false;
    p = std::move(daten_.front());
    daten_.erase(daten_.begin());
    return true;
}

bool Paketleser::NaechstesHashpaket(HashPaket& p) {
    if (hashes_.empty()) return false;
    p = hashes_.front();
    hashes_.erase(hashes_.begin());
    return true;
}

}
