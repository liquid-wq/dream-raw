// SD card: game list, menu image, backup and restore.

#include "core/gdiabbild.h"

#include "core/text.h"
#include "i18n.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <chrono>
#include <map>
#include <thread>

#include "core/hashcache.h"
#include "md5.h"

namespace fs = std::filesystem;

namespace core {
namespace {

constexpr uint32_t kTrack3 = 45000;

struct Tabellen {
    uint32_t edc[256];
    uint8_t ecc_f[256], ecc_b[256];
    Tabellen() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t e = i;
            for (int j = 0; j < 8; ++j) e = (e >> 1) ^ ((e & 1) ? 0xD8018001u : 0);
            edc[i] = e;
            const uint32_t f = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
            ecc_f[i] = static_cast<uint8_t>(f);
            ecc_b[i ^ f] = static_cast<uint8_t>(i);
        }
    }
};
const Tabellen& T() {
    static const Tabellen t;
    return t;
}

void EccBlock(const uint8_t* q, uint32_t major_n, uint32_t minor_n, uint32_t major_mult, uint32_t minor_inc,
              uint8_t* ziel) {
    const uint32_t groesse = major_n * minor_n;
    for (uint32_t major = 0; major < major_n; ++major) {
        uint32_t index = (major >> 1) * major_mult + (major & 1);
        uint8_t a = 0, b = 0;
        for (uint32_t minor = 0; minor < minor_n; ++minor) {
            const uint8_t v = q[index];
            index += minor_inc;
            if (index >= groesse) index -= groesse;
            a ^= v;
            b ^= v;
            a = T().ecc_f[a];
        }
        a = T().ecc_b[T().ecc_f[a] ^ b];
        ziel[major] = a;
        ziel[major + major_n] = a ^ b;
    }
}

uint8_t Bcd(uint32_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

void Mode1Sektor(uint8_t* s, const uint8_t* daten, uint32_t lba) {
    std::memset(s, 0, 2352);
    s[0] = 0;
    for (int i = 1; i <= 10; ++i) s[i] = 0xFF;
    s[11] = 0;
    const uint32_t a = lba + 150;
    s[12] = Bcd(a / (60 * 75));
    s[13] = Bcd((a / 75) % 60);
    s[14] = Bcd(a % 75);
    s[15] = 1;
    if (daten) std::memcpy(s + 16, daten, 2048);
    uint32_t edc = 0;
    for (int i = 0; i < 0x810; ++i) edc = (edc >> 8) ^ T().edc[(edc ^ s[i]) & 0xFF];
    s[0x810] = edc & 0xFF;
    s[0x811] = (edc >> 8) & 0xFF;
    s[0x812] = (edc >> 16) & 0xFF;
    s[0x813] = (edc >> 24) & 0xFF;
    EccBlock(s + 0xC, 86, 24, 2, 86, s + 0x81C);
    EccBlock(s + 0xC, 52, 43, 86, 88, s + 0x8C8);
}

void Le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF; }
void Be32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[3 - i] = (v >> (8 * i)) & 0xFF; }
void Beide32(uint8_t* p, uint32_t v) { Le32(p, v); Be32(p + 4, v); }
void Beide16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; p[2] = v >> 8; p[3] = v & 0xFF; }
void Text(uint8_t* p, size_t n, const std::string& s) {
    std::memset(p, ' ', n);
    std::memcpy(p, s.data(), std::min(n, s.size()));
}

size_t DirEintrag(uint8_t* p, uint32_t lba, uint32_t groesse, bool verzeichnis, const std::string& name) {
    const size_t nlen = name.size();
    size_t len = 33 + nlen;
    if (len & 1) ++len;
    std::memset(p, 0, len);
    p[0] = static_cast<uint8_t>(len);
    Beide32(p + 2, lba);
    Beide32(p + 10, groesse);
    p[18] = 126; p[19] = 11; p[20] = 16; p[21] = 12;
    p[25] = verzeichnis ? 2 : 0;
    Beide16(p + 28, 1);
    p[32] = static_cast<uint8_t>(nlen);
    std::memcpy(p + 33, name.data(), nlen);
    return len;
}

bool DateiSchreiben(const fs::path& pfad, const std::vector<uint8_t>& daten, std::string& fehler) {
    std::ofstream f(pfad, std::ios::binary | std::ios::trunc);
    if (!f) { fehler = Fmt(i18n::T("Kann nicht schreiben: %1"), {pfad.filename().string()}); return false; }
    f.write(reinterpret_cast<const char*>(daten.data()), static_cast<std::streamsize>(daten.size()));
    if (!f) { fehler = Fmt(i18n::T("Schreibfehler: %1"), {pfad.filename().string()}); return false; }
    return true;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t' || s.back() == 0))
        s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return s.substr(a);
}

bool GueltigesUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        const size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 9;
        if (n == 9) return false;
        for (size_t k = 1; k <= n; ++k)
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

std::string Latin1NachUtf8(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        if (c < 0x80) r += static_cast<char>(c);
        else { r += static_cast<char>(0xC0 | (c >> 6)); r += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return r;
}

}

bool GdiSchreiben(const std::wstring& ordner, const std::vector<uint8_t>& ipbin, std::vector<GdiDatei> dateien,
                  std::string& fehler) {
    if (ipbin.size() != 32768) { fehler = i18n::T("GDI nicht lesbar"); return false; }
    std::sort(dateien.begin(), dateien.end(), [](const GdiDatei& a, const GdiDatei& b) { return a.name < b.name; });

    constexpr uint32_t kEnde = 549150;
    const uint32_t pvd = 16, lpfad = 18, mpfad = 19, wurzel = 20;
    (void)pvd;
    uint32_t datei_sektoren = 0;
    for (const GdiDatei& d : dateien) datei_sektoren += std::max<uint32_t>(1, static_cast<uint32_t>((d.daten.size() + 2047) / 2048));
    const uint32_t spur5 = kEnde - datei_sektoren;
    const uint32_t spur4 = spur5 - 151;

    std::vector<uint32_t> datei_lba(dateien.size());
    {
        uint32_t naechster = spur5;
        for (size_t i = 0; i < dateien.size(); ++i) {
            datei_lba[i] = naechster;
            naechster += std::max<uint32_t>(1, static_cast<uint32_t>((dateien[i].daten.size() + 2047) / 2048));
        }
    }

    std::vector<uint8_t> dir;
    uint32_t dir_sektoren = 1;
    for (int durchlauf = 0; durchlauf < 2; ++durchlauf) {
        dir.assign(2048 * 64, 0);
        size_t pos = 0;
        uint8_t tmp[256];
        auto anfuegen = [&](size_t len) {
            if ((pos % 2048) + len > 2048) pos = (pos / 2048 + 1) * 2048;
            std::memcpy(&dir[pos], tmp, len);
            pos += len;
        };
        const uint32_t dir_groesse = dir_sektoren * 2048;
        anfuegen(DirEintrag(tmp, kTrack3 + wurzel, dir_groesse, true, std::string(1, '\0')));
        anfuegen(DirEintrag(tmp, kTrack3 + wurzel, dir_groesse, true, std::string(1, '\1')));
        for (size_t i = 0; i < dateien.size(); ++i)
            anfuegen(DirEintrag(tmp, datei_lba[i], static_cast<uint32_t>(dateien[i].daten.size()), false,
                                dateien[i].name + ";1"));
        const uint32_t noetig = static_cast<uint32_t>((pos + 2047) / 2048);
        if (noetig == dir_sektoren) break;
        dir_sektoren = noetig;
    }
    dir.resize(dir_sektoren * 2048);

    std::vector<uint8_t> p(2048, 0);
    p[0] = 1;
    std::memcpy(&p[1], "CD001", 5);
    p[6] = 1;
    Text(&p[8], 32, "SEGA SEGAKATANA");
    Text(&p[40], 32, "DREAM_RAW");
    Beide32(&p[80], kEnde);
    Beide16(&p[120], 1);
    Beide16(&p[124], 1);
    Beide16(&p[128], 2048);
    Beide32(&p[132], 10);
    Le32(&p[140], kTrack3 + lpfad);
    Be32(&p[148], kTrack3 + mpfad);
    DirEintrag(&p[156], kTrack3 + wurzel, dir_sektoren * 2048, true, std::string(1, '\0'));
    Text(&p[190], 128, "");
    Text(&p[318], 128, "DC-RAW");
    Text(&p[446], 128, "DC-RAW");
    Text(&p[574], 128, "DC-RAW");
    Text(&p[702], 37, "");
    Text(&p[739], 37, "");
    Text(&p[776], 37, "");
    const char* datum = "2026111612000000";
    std::memcpy(&p[813], datum, 16);
    std::memcpy(&p[830], datum, 16);
    std::memcpy(&p[847], "0000000000000000", 16);
    std::memcpy(&p[864], "0000000000000000", 16);
    p[881] = 1;

    std::vector<uint8_t> term(2048, 0);
    term[0] = 255;
    std::memcpy(&term[1], "CD001", 5);
    term[6] = 1;

    std::vector<uint8_t> lp(2048, 0), mp(2048, 0);
    lp[0] = 1; Le32(&lp[2], kTrack3 + wurzel); lp[6] = 1;
    mp[0] = 1; Be32(&mp[2], kTrack3 + wurzel); mp[7] = 1;

    const fs::path basis(ordner);
    auto iso_schreiben = [&](const wchar_t* name, const std::vector<const uint8_t*>& sektoren) {
        std::ofstream f(basis / name, std::ios::binary | std::ios::trunc);
        static const uint8_t leer[2048] = {};
        for (const uint8_t* s : sektoren) f.write(reinterpret_cast<const char*>(s ? s : leer), 2048);
        return static_cast<bool>(f);
    };

    std::vector<const uint8_t*> s3;
    for (uint32_t i = 0; i < 16; ++i) s3.push_back(&ipbin[i * 2048]);
    s3.push_back(p.data());
    s3.push_back(term.data());
    s3.push_back(lp.data());
    s3.push_back(mp.data());
    for (uint32_t i = 0; i < dir_sektoren; ++i) s3.push_back(&dir[i * 2048]);
    if (!iso_schreiben(L"track03.iso", s3)) { fehler = "Schreibfehler track03.iso"; return false; }

    std::vector<std::vector<uint8_t>> bloecke;
    for (const GdiDatei& d : dateien) {
        const size_t n = std::max<size_t>(1, (d.daten.size() + 2047) / 2048);
        for (size_t i = 0; i < n; ++i) {
            std::vector<uint8_t> blk(2048, 0);
            const size_t von = i * 2048;
            if (von < d.daten.size()) std::memcpy(blk.data(), &d.daten[von], std::min<size_t>(2048, d.daten.size() - von));
            bloecke.push_back(std::move(blk));
        }
    }
    std::vector<const uint8_t*> s5;
    for (const auto& b : bloecke) s5.push_back(b.data());
    if (!iso_schreiben(L"track05.iso", s5)) { fehler = "Schreibfehler track05.iso"; return false; }

    if (!iso_schreiben(L"track01.iso", std::vector<const uint8_t*>(37, nullptr))) { fehler = "Schreibfehler track01.iso"; return false; }

    const std::vector<uint8_t> still(2352, 0);
    if (!DateiSchreiben(basis / L"track02.raw", still, fehler)) return false;
    if (!DateiSchreiben(basis / L"track04.raw", still, fehler)) return false;
    const std::string gdi = "5\r\n1 0 4 2048 track01.iso 0\r\n2 450 0 2352 track02.raw 0\r\n3 45000 4 2048 track03.iso 0\r\n4 " +
                            std::to_string(spur4) + " 0 2352 track04.raw 0\r\n5 " + std::to_string(spur5) +
                            " 4 2048 track05.iso 0\r\n";
    return DateiSchreiben(basis / L"disc.gdi", std::vector<uint8_t>(gdi.begin(), gdi.end()), fehler);
}
bool IpBinAusGdi(const std::wstring& gdi_pfad, std::vector<uint8_t>& ipbin, std::string& fehler) {
    std::ifstream g{fs::path(gdi_pfad)};
    if (!g) { fehler = i18n::T("GDI nicht lesbar"); return false; }
    std::string zeile;
    std::getline(g, zeile);
    while (std::getline(g, zeile)) {
        unsigned nr = 0, lba = 0, typ = 0, groesse = 0;
        char name[512] = {};

        const size_t q1 = zeile.find('"');
        if (q1 != std::string::npos) {
            const size_t q2 = zeile.find('"', q1 + 1);
            if (std::sscanf(zeile.c_str(), "%u %u %u %u", &nr, &lba, &typ, &groesse) != 4 || q2 == std::string::npos)
                continue;
            std::snprintf(name, sizeof(name), "%s", zeile.substr(q1 + 1, q2 - q1 - 1).c_str());
        } else if (std::sscanf(zeile.c_str(), "%u %u %u %u %511s", &nr, &lba, &typ, &groesse, name) != 5) {
            continue;
        }
        if (lba != kTrack3 || typ != 4) continue;
        const fs::path spur = fs::path(gdi_pfad).parent_path() / fs::u8path(name);
        std::ifstream f(spur, std::ios::binary);
        if (!f) { fehler = i18n::T("GDI nicht lesbar"); return false; }
        const size_t versatz = groesse == 2352 ? 16 : 0;
        ipbin.assign(32768, 0);
        for (int i = 0; i < 16; ++i) {
            f.seekg(static_cast<std::streamoff>(i) * groesse + versatz);
            f.read(reinterpret_cast<char*>(&ipbin[i * 2048]), 2048);
        }
        if (!f || std::memcmp(ipbin.data(), "SEGA SEGAKATANA ", 16) != 0) {
            fehler = i18n::T("GDI nicht lesbar");
            return false;
        }
        return true;
    }
    fehler = i18n::T("GDI nicht lesbar");
    return false;
}

namespace {

bool Spur3Finden(const std::wstring& gdi_pfad, fs::path& spur, unsigned& groesse) {
    std::ifstream g{fs::path(gdi_pfad)};
    std::string zeile;
    if (!g || !std::getline(g, zeile)) return false;
    while (std::getline(g, zeile)) {
        unsigned nr = 0, lba = 0, typ = 0, sg = 0;
        char name[512] = {};
        const size_t q1 = zeile.find('"');
        if (q1 != std::string::npos) {
            const size_t q2 = zeile.find('"', q1 + 1);
            if (std::sscanf(zeile.c_str(), "%u %u %u %u", &nr, &lba, &typ, &sg) != 4 || q2 == std::string::npos) continue;
            std::snprintf(name, sizeof(name), "%s", zeile.substr(q1 + 1, q2 - q1 - 1).c_str());
        } else if (std::sscanf(zeile.c_str(), "%u %u %u %u %511s", &nr, &lba, &typ, &sg, name) != 5) {
            continue;
        }
        if (lba != kTrack3 || typ != 4) continue;
        spur = fs::path(gdi_pfad).parent_path() / fs::u8path(name);
        groesse = sg;
        return true;
    }
    return false;
}

bool SektorenLesen(std::ifstream& f, unsigned groesse, uint32_t rel_sektor, uint32_t anzahl, uint8_t* ziel) {

    const size_t versatz = groesse == 2352 ? 16 : 0;
    std::vector<char> block;
    for (uint32_t i = 0; i < anzahl;) {
        const uint32_t n = std::min<uint32_t>(256, anzahl - i);
        block.resize(static_cast<size_t>(n) * groesse);
        f.seekg(static_cast<std::streamoff>(rel_sektor + i) * groesse);
        f.read(block.data(), static_cast<std::streamsize>(block.size()));
        if (!f) return false;
        for (uint32_t j = 0; j < n; ++j)
            std::memcpy(ziel + static_cast<size_t>(i + j) * 2048, &block[static_cast<size_t>(j) * groesse + versatz], 2048);
        i += n;
    }
    return true;
}
}

namespace {

struct GdiLeser {
    struct Spur { uint32_t lba; unsigned sg; fs::path pfad; };
    std::vector<Spur> spuren;
    bool Oeffnen(const std::wstring& gdi_pfad) {
        std::ifstream g{fs::path(gdi_pfad)};
        std::string zeile;
        if (!g || !std::getline(g, zeile)) return false;
        while (std::getline(g, zeile)) {
            unsigned nr = 0, lba = 0, typ = 0, sg = 0;
            char name[512] = {};
            const size_t q1 = zeile.find('"');
            if (q1 != std::string::npos) {
                const size_t q2 = zeile.find('"', q1 + 1);
                if (std::sscanf(zeile.c_str(), "%u %u %u %u", &nr, &lba, &typ, &sg) != 4 || q2 == std::string::npos) continue;
                std::snprintf(name, sizeof(name), "%s", zeile.substr(q1 + 1, q2 - q1 - 1).c_str());
            } else if (std::sscanf(zeile.c_str(), "%u %u %u %u %511s", &nr, &lba, &typ, &sg, name) != 5) {
                continue;
            }
            if (typ == 4 && (sg == 2048 || sg == 2352))
                spuren.push_back({lba, sg, fs::path(gdi_pfad).parent_path() / fs::u8path(name)});
        }
        return !spuren.empty();
    }
    bool Lesen(uint32_t lba, uint32_t anzahl, uint8_t* ziel) const {
        const Spur* s = nullptr;
        for (const Spur& k : spuren)
            if (k.lba <= lba && (!s || k.lba > s->lba)) s = &k;
        if (!s) return false;
        std::ifstream f(s->pfad, std::ios::binary);
        return f && SektorenLesen(f, s->sg, lba - s->lba, anzahl, ziel);
    }
};
}

bool StartdateiAusGdi(const std::wstring& gdi_pfad, std::vector<uint8_t>& ip, std::vector<uint8_t>& d,
                      std::string& boot, std::string& fehler) {
    GdiLeser gl;
    if (!gl.Oeffnen(gdi_pfad)) { fehler = i18n::T("GDI nicht lesbar"); return false; }
    std::vector<uint8_t> s(2048);
    ip.assign(32768, 0);
    if (!gl.Lesen(kTrack3, 16, ip.data()) || std::memcmp(ip.data(), "SEGA SEGAKATANA ", 16) != 0) {
        fehler = i18n::T("GDI nicht lesbar");
        return false;
    }
    boot.assign(reinterpret_cast<const char*>(&ip[0x60]), 16);
    const size_t leer = boot.find_first_of(" \0", 0, 2);
    if (leer != std::string::npos) boot.resize(leer);
    if (!gl.Lesen(kTrack3 + 16, 1, s.data()) || std::memcmp(&s[1], "CD001", 5) != 0) { fehler = i18n::T("GDI nicht lesbar"); return false; }
    auto le32 = [](const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); };
    const uint32_t dir_lba = le32(&s[158]), dir_len = le32(&s[166]);
    uint32_t datei_lba = 0, datei_len = 0;
    bool gefunden = false;
    for (uint32_t k = 0; k < (dir_len + 2047) / 2048 && !gefunden; ++k) {
        if (!gl.Lesen(dir_lba + k, 1, s.data())) break;
        for (size_t pos = 0; pos < 2048 && s[pos];) {
            const uint8_t len = s[pos], nl = s[pos + 32];
            std::string n(reinterpret_cast<const char*>(&s[pos + 33]), nl);
            const size_t semi = n.find(';');
            if (semi != std::string::npos) n.resize(semi);
            if (_stricmp(n.c_str(), boot.c_str()) == 0) {
                datei_lba = le32(&s[pos + 2]);
                datei_len = le32(&s[pos + 10]);
                gefunden = true;
                break;
            }
            pos += len;
        }
    }
    if (!gefunden) { fehler = Fmt(i18n::T("Startdatei %1 nicht gefunden"), {boot}); return false; }
    if (datei_len > 64u * 1024 * 1024) { fehler = i18n::T("Startdatei zu groß"); return false; }
    d.assign(((datei_len + 2047) / 2048) * 2048, 0);
    if (!gl.Lesen(datei_lba, static_cast<uint32_t>(d.size() / 2048), d.data())) { fehler = i18n::T("Startdatei nicht lesbar"); return false; }
    d.resize(datei_len);
    return true;
}

bool RaHashesAusGdi(const std::wstring& gdi_pfad, std::vector<std::string>& hashes, std::string& fehler) {
    hashes.clear();
    GdiLeser gl;
    if (!gl.Oeffnen(gdi_pfad)) { fehler = i18n::T("GDI nicht lesbar"); return false; }
    std::vector<uint8_t> ip(32768), s(2048);
    if (!gl.Lesen(kTrack3, 16, ip.data()) || std::memcmp(ip.data(), "SEGA SEGAKATANA ", 16) != 0) {
        fehler = i18n::T("GDI nicht lesbar");
        return false;
    }
    std::string boot(reinterpret_cast<const char*>(&ip[0x60]), 16);
    const size_t leer = boot.find_first_of(" \0", 0, 2);
    if (leer != std::string::npos) boot.resize(leer);

    if (!gl.Lesen(kTrack3 + 16, 1, s.data()) || std::memcmp(&s[1], "CD001", 5) != 0) { fehler = i18n::T("GDI nicht lesbar"); return false; }
    auto le32 = [](const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); };
    const uint32_t dir_lba = le32(&s[158]), dir_len = le32(&s[166]);
    uint32_t datei_lba = 0, datei_len = 0;
    bool gefunden = false;
    for (uint32_t k = 0; k < (dir_len + 2047) / 2048 && !gefunden; ++k) {
        if (!gl.Lesen(dir_lba + k, 1, s.data())) break;
        for (size_t pos = 0; pos < 2048 && s[pos];) {
            const uint8_t len = s[pos], nl = s[pos + 32];
            std::string n(reinterpret_cast<const char*>(&s[pos + 33]), nl);
            const size_t semi = n.find(';');
            if (semi != std::string::npos) n.resize(semi);
            if (_stricmp(n.c_str(), boot.c_str()) == 0) {
                datei_lba = le32(&s[pos + 2]);
                datei_len = le32(&s[pos + 10]);
                gefunden = true;
                break;
            }
            pos += len;
        }
    }
    if (!gefunden) { fehler = Fmt(i18n::T("Startdatei %1 nicht gefunden"), {boot}); return false; }
    if (datei_len > 64u * 1024 * 1024) { fehler = i18n::T("Startdatei zu groß"); return false; }
    std::vector<uint8_t> d(((datei_len + 2047) / 2048) * 2048);
    if (!gl.Lesen(datei_lba, static_cast<uint32_t>(d.size() / 2048), d.data())) {
        fehler = i18n::T("Startdatei nicht lesbar");
        return false;
    }    static const char* regionen[7] = {"J       ", " U      ", "  E     ", "JU      ", "J E     ", " UE     ", "JUE     "};
    for (int v = 0; v < 15; ++v) {
        uint8_t kopf[256];
        std::memcpy(kopf, ip.data(), 256);
        if (v > 0) {
            const int r = (v - 1) % 7, vga = (v - 1) / 7;
            std::memcpy(kopf + 0x30, regionen[r], 8);
            const char c = static_cast<char>(kopf[0x3D]);
            const int z = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (z >= 0) kopf[0x3D] = static_cast<uint8_t>("0123456789ABCDEF"[vga ? (z | 1) : (z & ~1)]);
        }
        md5_state_t m;
        md5_byte_t dig[16];
        md5_init(&m);
        md5_append(&m, kopf, 256);
        md5_append(&m, d.data(), static_cast<int>(datei_len));
        md5_finish(&m, dig);
        char h[33];
        for (int i = 0; i < 16; ++i) std::snprintf(h + 2 * i, 3, "%02x", dig[i]);
        hashes.push_back(h);
    }
    return true;
}

namespace {
std::vector<uint8_t> g_startlogo;
void Le32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
}

void StartlogoVorgeben(std::vector<uint8_t> mr) { g_startlogo = std::move(mr); }

std::vector<uint8_t> MrKodieren(const std::vector<uint8_t>& index, int w, int h, const std::vector<uint32_t>& palette_rgb) {
    if (w <= 0 || h <= 0 || index.size() != static_cast<size_t>(w) * h || palette_rgb.empty() || palette_rgb.size() > 128)
        return {};
    std::vector<uint8_t> daten;
    for (size_t i = 0; i < index.size();) {
        const uint8_t v = index[i];
        if (v >= 0x80) return {};
        size_t lauf = 1;
        while (i + lauf < index.size() && index[i + lauf] == v && lauf < 0x17F) ++lauf;
        if (lauf == 1) {
            daten.push_back(v);
        } else if (lauf < 0x80) {
            daten.push_back(static_cast<uint8_t>(0x80 | lauf)); daten.push_back(v);
        } else if (lauf < 0x100) {
            daten.push_back(0x81); daten.push_back(static_cast<uint8_t>(lauf)); daten.push_back(v);
        } else {
            daten.push_back(0x82); daten.push_back(static_cast<uint8_t>(0x80 | (lauf - 0x100))); daten.push_back(v);
        }
        i += lauf;
    }
    const uint32_t beginn = 30 + 4 * static_cast<uint32_t>(palette_rgb.size());
    std::vector<uint8_t> mr = {'M', 'R'};
    Le32(mr, beginn + static_cast<uint32_t>(daten.size()));
    Le32(mr, 0);
    Le32(mr, beginn);
    Le32(mr, static_cast<uint32_t>(w));
    Le32(mr, static_cast<uint32_t>(h));
    Le32(mr, 0);
    Le32(mr, static_cast<uint32_t>(palette_rgb.size()));
    for (uint32_t c : palette_rgb) {
        mr.push_back(static_cast<uint8_t>(c));
        mr.push_back(static_cast<uint8_t>(c >> 8));
        mr.push_back(static_cast<uint8_t>(c >> 16));
        mr.push_back(0);
    }
    mr.insert(mr.end(), daten.begin(), daten.end());
    if (mr.size() > kMrMax) return {};
    return mr;
}

bool IpLogoEinsetzen(std::vector<uint8_t>& ipbin, const std::vector<uint8_t>& mr) {
    if (mr.empty() || mr.size() > kMrMax || ipbin.size() < kMrOffset + kMrMax) return false;
    uint8_t* p = &ipbin[kMrOffset];
    if (p[0] == 'M' && p[1] == 'R') {
        const uint32_t alt = p[2] | (p[3] << 8) | (p[4] << 16) | (static_cast<uint32_t>(p[5]) << 24);
        std::memset(p, 0, std::min<size_t>(alt, kMrMax));
    }
    std::memcpy(p, mr.data(), mr.size());
    return true;
}

void IpBinAnpassen(std::vector<uint8_t>& ipbin, const std::string& startdatei, const std::string& titel,
                   const std::string& produkt) {
    if (ipbin.size() < 256) return;
    if (!produkt.empty()) Text(&ipbin[0x40], 10, produkt);
    Text(&ipbin[0x60], 16, startdatei);
    Text(&ipbin[0x80], 128, titel);
}

static bool EigenesMenue(const fs::path& p) {
    std::error_code e;
    return fs::exists(p / L"dc-raw.txt", e) || fs::exists(p / L"dream-raw.txt", e);
}

std::vector<KartenPlatz> KarteLesen(const std::wstring& wurzel) {
    std::vector<KartenPlatz> r;
    std::error_code ec;

    std::map<std::string, std::string> ip_cache;
    bool ip_cache_neu = false;
    fs::path ip_cache_datei;
    if (const wchar_t* a = _wgetenv(L"APPDATA")) {
        ip_cache_datei = fs::path(a) / L"DREAM-RAW" / L"ip_titel_cache.txt";
        std::ifstream c(ip_cache_datei, std::ios::binary);
        std::string z;
        while (std::getline(c, z)) {
            const size_t tab = z.find('\t');
            if (tab != std::string::npos) ip_cache[z.substr(0, tab)] = z.substr(tab + 1);
        }
    }
    for (const auto& e : fs::directory_iterator(fs::path(wurzel), ec)) {
        if (!e.is_directory(ec)) continue;
        const std::wstring n = e.path().filename().wstring();
        if (n.empty() || n.size() > 3 || !std::all_of(n.begin(), n.end(), iswdigit)) continue;
        KartenPlatz k;
        k.platz = std::stoi(n);
        if (k.platz < 1 || k.platz > 999) continue;
        uintmax_t summe_groessen = 0;
        for (const auto& d : fs::directory_iterator(e.path(), ec)) {
            std::error_code ec2;
            if (d.is_regular_file(ec2)) summe_groessen += d.file_size(ec2);
            std::wstring ext = d.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
            if (ext == L".gdi") { k.format = "GDI"; k.abbild = d.path().wstring(); }
            else if (ext == L".cdi" && k.format != "GDI") { k.format = "CDI"; k.abbild = d.path().wstring(); }
        }
        k.eigenes_menue = EigenesMenue(e.path());
        std::ifstream nt(e.path() / L"name.txt", std::ios::binary);
        if (nt) {
            std::string s;
            std::getline(nt, s);
            if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF) s = s.substr(3);
            s = Trim(s);
            k.titel = GueltigesUtf8(s) ? s : Latin1NachUtf8(s);
        }
        if (k.format == "GDI" && !k.eigenes_menue) {
            std::error_code ec2;
            const auto zeit = fs::last_write_time(fs::path(k.abbild), ec2).time_since_epoch().count();
            std::string schluessel;
            for (wchar_t c : k.abbild) schluessel += (c < 0x80 ? (char)c : '?');
            schluessel += "|" + std::to_string(zeit) + "|" + std::to_string(summe_groessen);
            std::string ip_titel;
            bool da = false;
            const auto it = ip_cache.find(schluessel);
            if (it != ip_cache.end()) { ip_titel = it->second; da = true; }
            else {
                std::vector<uint8_t> ip;
                std::string f;
                if (IpBinAusGdi(k.abbild, ip, f)) {
                    ip_titel = Trim(std::string(ip.begin() + 0x80, ip.begin() + 0x100));
                    for (char& c : ip_titel) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
                    ip_cache[schluessel] = ip_titel;
                    ip_cache_neu = true;
                    da = true;
                }
            }
            if (da) {
                k.platz_hoch = ZweiterPlatzNoetig(ip_titel);
                if (k.titel.empty()) k.titel = Latin1NachUtf8(ip_titel);
            }
        }
        if (k.titel.empty()) k.titel = "Platz " + std::to_string(k.platz);
        r.push_back(std::move(k));
    }
    std::sort(r.begin(), r.end(), [](const KartenPlatz& a, const KartenPlatz& b) { return a.platz < b.platz; });
    if (ip_cache_neu && !ip_cache_datei.empty()) {
        fs::create_directories(ip_cache_datei.parent_path(), ec);
        std::ofstream c(ip_cache_datei, std::ios::binary | std::ios::trunc);
        for (const auto& kv : ip_cache) c << kv.first << '\t' << kv.second << '\n';
    }
    return r;
}

namespace {

std::wstring AppdataDatei(const wchar_t* name) {
    const wchar_t* a = _wgetenv(L"APPDATA");
    if (!a) return L"";
    const fs::path o = fs::path(a) / L"DREAM-RAW";
    std::error_code ec;
    fs::create_directories(o, ec);
    return (o / name).wstring();
}

std::string AbbildSchluessel(const std::wstring& gdi) {
    std::error_code ec;
    uint64_t groesse = 0;
    long long zeit = 0;
    for (const auto& e : fs::directory_iterator(fs::path(gdi).parent_path(), ec)) {
        if (!e.is_regular_file(ec)) continue;
        groesse += e.file_size(ec);
        zeit = std::max<long long>(zeit, e.last_write_time(ec).time_since_epoch().count());
    }
    return fs::path(gdi).u8string() + "|" + std::to_string(groesse) + "|" + std::to_string(zeit);
}

struct AbbildHashes {
    std::map<std::string, std::vector<std::string>> eintraege;

    std::wstring pfad = AppdataDatei(L"abbild_hashes_2.txt");
    AbbildHashes() {
        std::ifstream f{fs::path(pfad)};
        std::string z;
        while (std::getline(f, z)) {
            const size_t tab = z.find('\t');
            if (tab == std::string::npos) continue;
            std::vector<std::string> h;
            for (size_t p = tab + 1; p + 32 <= z.size(); p += 33) h.push_back(z.substr(p, 32));
            if (h.size() == 15) eintraege[z.substr(0, tab)] = h;
        }
    }
    void Speichern() const {
        if (pfad.empty()) return;
        const fs::path tmp = fs::path(pfad + L".tmp");
        {
            std::ofstream f(tmp, std::ios::trunc);
            for (const auto& e : eintraege) {
                f << e.first << '\t';
                for (size_t i = 0; i < e.second.size(); ++i) f << e.second[i] << (i + 1 < e.second.size() ? "," : "");
                f << '\n';
            }
            if (!f) return;
        }
        std::error_code ec;
        fs::rename(tmp, fs::path(pfad), ec);
    }
};

}

RaPruefung RaStatusBestimmen(std::vector<KartenPlatz>& plaetze, bool netz, const volatile bool* abbruch,
                             const std::function<void(const std::string&)>& meldung) {
    RaPruefung r;
    AbbildHashes abh;
    bool geaendert = false;

    HashBibliothek bib;
    if (netz) {
        if (meldung) meldung("Lade die Liste aller Dreamcast-Hashes von RA ...");
        bib = HashBibliothekLaden(kKonsoleDreamcast);
        ++r.abfragen;
        if (meldung)
            meldung(bib.ok ? Fmt(i18n::T("%1 bekannte Hashes geladen."), {std::to_string(bib.hashes.size())})
                           : Fmt(i18n::T("Liste nicht geladen (%1), frage einzeln."), {i18n::T(bib.fehler)}));
    }
    std::map<std::string, uint32_t> fuer_cache;
    auto letzte = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    size_t nr = 0;
    for (KartenPlatz& k : plaetze) {
        ++nr;
        if (abbruch && *abbruch) { r.abgebrochen = true; break; }
        if (k.format != "GDI") continue;
        const std::string schluessel = AbbildSchluessel(k.abbild);
        auto it = abh.eintraege.find(schluessel);
        if (it == abh.eintraege.end()) {
            if (meldung) meldung(Fmt(i18n::T("Lese Platz %1 (%2/%3) ..."),
                                     {std::to_string(k.platz), std::to_string(nr), std::to_string(plaetze.size())}));
            std::vector<std::string> h;
            std::string f;
            if (!RaHashesAusGdi(k.abbild, h, f)) { k.ra = '?'; ++r.offen; continue; }
            it = abh.eintraege.emplace(schluessel, h).first;
            geaendert = true;
        }
        const std::vector<std::string>& h = it->second;

        bool alle_bekannt = true;
        for (const std::string& x : h) {
            uint32_t id = 0;
            if (!HashCache::Global().Nachsehen(x, id)) { alle_bekannt = false; continue; }
            if (id) { k.ra = '1'; k.spiel_id = id; break; }
        }
        if (k.ra != '1' && !alle_bekannt && bib.ok) {

            for (const std::string& x : h) {
                auto b = bib.hashes.find(x);
                if (b != bib.hashes.end() && b->second) {
                    fuer_cache[x] = b->second;
                    k.ra = '1';
                    k.spiel_id = b->second;
                }
            }

            if (meldung && k.ra == '1') meldung(Fmt(i18n::T("Platz %1: RA kennt %2"), {std::to_string(k.platz), k.titel}));
        }
        if (k.ra != '1' && !alle_bekannt && netz && !bib.ok && r.fehler.empty()) {
            for (const std::string& x : h) {
                uint32_t id = 0;
                if (HashCache::Global().Nachsehen(x, id)) continue;
                if (abbruch && *abbruch) break;
                const auto seit = std::chrono::steady_clock::now() - letzte;
                if (seit < std::chrono::milliseconds(1100))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1100) - seit);
                const HashAufloesung a = HashCache::Global().Aufloesen(x);
                letzte = std::chrono::steady_clock::now();
                ++r.abfragen;
                if (!a.ok) { r.fehler = a.fehler; break; }
                if (a.spiel_id) { k.ra = '1'; k.spiel_id = a.spiel_id; break; }
            }
            if (k.ra != '1' && r.fehler.empty() && !(abbruch && *abbruch)) alle_bekannt = true;
            if (meldung && k.ra == '1') meldung(Fmt(i18n::T("Platz %1: RA kennt %2"), {std::to_string(k.platz), k.titel}));
        }
        if (k.ra == '1') ++r.bekannt;
        else if (alle_bekannt) { k.ra = '0'; ++r.unbekannt; }
        else ++r.offen;
    }
    if (geaendert) abh.Speichern();
    HashCache::Global().Eintragen(fuer_cache);
    return r;
}

std::vector<uint8_t> IconsBinBauen(const std::vector<IconBild>& icons) {
    std::vector<uint8_t> d = {'D', 'R', 'I', 'C'};
    auto u16 = [&](uint32_t v) { d.push_back(v & 0xFF); d.push_back((v >> 8) & 0xFF); };
    std::vector<const IconBild*> gueltig;
    for (const IconBild& i : icons)
        if (i.rgb565.size() == static_cast<size_t>(kIconKante) * kIconKante && i.platz > 0) gueltig.push_back(&i);
    u16(static_cast<uint32_t>(gueltig.size()));
    u16(kIconKante);
    for (const IconBild* i : gueltig) { u16(static_cast<uint32_t>(i->platz)); u16(0); }
    for (const IconBild* i : gueltig)
        for (uint16_t p : i->rgb565) u16(p);
    return d;
}

bool ZweiterPlatzNoetig(const std::string& ip_titel) {
    static const char* const kHoch[] = {"18WHEELER", "CRAZY TAXI"};
    for (const char* t : kHoch)
        if (ip_titel == t) return true;
    return false;
}

std::string SpielelisteText(const std::vector<KartenPlatz>& plaetze, int menue_platz) {
    std::string s = "DC-RAW-LISTE 3\n";
    for (const KartenPlatz& k : plaetze) {
        if (k.platz == menue_platz || k.format.empty() || k.eigenes_menue) continue;
        std::string t = k.titel;
        std::replace(t.begin(), t.end(), '|', '/');
        s += std::to_string(k.platz) + "|" + k.format + "|" + std::string(1, k.ra) + "|" + std::to_string(k.spiel_id) +
             "|" + (k.platz_hoch ? "1" : "0") + "|" + t + "\n";
    }
    return s;
}

namespace {
bool DateiLesen(const fs::path& p, std::vector<uint8_t>& d) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    d.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::vector<fs::path> Dateiliste(const fs::path& ordner) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(ordner, ec)) v.push_back(e.path());
    return v;
}
std::wstring PlatzName(int platz) {
    wchar_t b[8];
    std::swprintf(b, 8, L"%02d", platz);
    return b;
}
}

int SpurenUmbenennen(const std::wstring& gdi_pfad, std::string& meldung) {
    const fs::path gdi(gdi_pfad);
    const fs::path ordner = gdi.parent_path();
    std::ifstream f(gdi);
    if (!f) { meldung = i18n::T("GDI nicht lesbar"); return -1; }
    std::vector<std::string> zeilen;
    for (std::string z; std::getline(f, z);) {
        if (!z.empty() && z.back() == '\r') z.pop_back();
        if (!Trim(z).empty()) zeilen.push_back(z);
    }
    f.close();
    if (zeilen.empty()) { meldung = "GDI leer"; return -1; }
    int anzahl = 0;
    try { anzahl = std::stoi(Trim(zeilen[0])); } catch (...) { meldung = "GDI-Kopf unlesbar"; return -1; }
    if (anzahl <= 5) return 0;

    struct Spur { int nr = 0; std::string lba, typ, sg, alt, neu, versatz; };
    std::vector<Spur> spuren;
    for (size_t i = 1; i < zeilen.size(); ++i) {
        std::istringstream is(zeilen[i]);
        Spur s;
        int typ = 0, sg = 0;
        is >> s.nr >> s.lba >> typ >> sg >> std::ws;
        if (!is) { meldung = "GDI-Zeile unlesbar"; return -1; }
        if (is.peek() == '"') { is.get(); std::getline(is, s.alt, '"'); }
        else is >> s.alt;
        is >> s.versatz;
        if (s.alt.empty() || s.versatz.empty()) { meldung = "GDI-Zeile unlesbar"; return -1; }
        s.typ = std::to_string(typ);
        s.sg = std::to_string(sg);
        char b[24];
        std::snprintf(b, sizeof(b), "track%02d.%s", s.nr, typ == 0 ? "raw" : (sg == 2048 ? "iso" : "bin"));
        s.neu = b;
        spuren.push_back(s);
    }
    if (static_cast<int>(spuren.size()) != anzahl) { meldung = i18n::T("Spurzahl passt nicht zum GDI-Kopf"); return -1; }
    std::error_code ec;
    for (const Spur& s : spuren)
        if (!fs::exists(ordner / fs::u8path(s.alt), ec)) { meldung = "Spurdatei fehlt: " + s.alt; return -1; }
    bool noetig = gdi.filename() != L"disc.gdi";
    for (const Spur& s : spuren) noetig = noetig || s.alt != s.neu;
    if (!noetig) return 0;

    for (const Spur& s : spuren) {
        if (s.alt == s.neu) continue;
        fs::rename(ordner / fs::u8path(s.alt), ordner / (L"_um_" + std::to_wstring(s.nr) + L".tmp"), ec);
        if (ec) { meldung = "Umbenennen fehlgeschlagen: " + s.alt; return -1; }
    }
    for (const Spur& s : spuren) {
        if (s.alt == s.neu) continue;
        fs::rename(ordner / (L"_um_" + std::to_wstring(s.nr) + L".tmp"), ordner / fs::u8path(s.neu), ec);
        if (ec) { meldung = "Umbenennen fehlgeschlagen: " + s.neu; return -1; }
    }
    fs::copy_file(gdi, ordner / L"gdialt.txt", fs::copy_options::overwrite_existing, ec);
    {
        const std::string n = gdi.filename().u8string();
        std::string f2;
        DateiSchreiben(ordner / L"gdialt_name.txt", {n.begin(), n.end()}, f2);
    }
    std::string neu = std::to_string(anzahl) + "\r\n";
    for (const Spur& s : spuren)
        neu += std::to_string(s.nr) + " " + s.lba + " " + s.typ + " " + s.sg + " " + s.neu + " " + s.versatz + "\r\n";
    if (gdi.filename() != L"disc.gdi") fs::remove(gdi, ec);
    std::string fehler;
    if (!DateiSchreiben(ordner / L"disc.gdi", {neu.begin(), neu.end()}, fehler)) { meldung = fehler; return -1; }
    meldung = std::to_string(anzahl) + " Spuren";
    return 1;
}

MenueErgebnis MenueAufKarte(const std::wstring& wurzel, int platz, const std::vector<uint8_t>& erstlauf,
                            bool ra_status, const ZusatzDateien& zusatz,
                            const std::function<void(const std::string&)>& fortschritt) {
    auto melde = [&](const std::string& s) { if (fortschritt) fortschritt(s); };
    MenueErgebnis r;
    if (erstlauf.empty()) { r.fehler = i18n::T("DC-RAW-Programm fehlt in diesem Build"); return r; }
    if (platz < 1 || platz > 999) { r.fehler = i18n::T("Platz muss zwischen 1 und 999 liegen"); return r; }
    const fs::path karte(wurzel);
    std::error_code ec;
    if (!fs::is_directory(karte, ec)) { r.fehler = i18n::T("Laufwerk nicht gefunden"); return r; }

    melde("Schritt 1/5: lese die Plätze der Karte ...");
    std::vector<KartenPlatz> plaetze = KarteLesen(wurzel);
    melde(Fmt(i18n::T("%1 Plätze gefunden."), {std::to_string(plaetze.size())}));

    {
        int umbenannt = 0;
        for (KartenPlatz& k : plaetze) {
            if (k.format != "GDI" || k.eigenes_menue) continue;
            std::string m;
            const int e = SpurenUmbenennen(k.abbild, m);
            if (e == 1) {
                ++umbenannt;
                k.abbild = (fs::path(k.abbild).parent_path() / L"disc.gdi").wstring();
                melde(Fmt(i18n::T("Platz %1: Spurdateien umbenannt (%2)"), {std::to_string(k.platz), m}));
            } else if (e < 0) {
                r.meldungen.push_back(Fmt(i18n::T("Platz %1: Spurdateien nicht umbenannt (%2)"), {std::to_string(k.platz), m}));
            }
        }
        if (umbenannt)
            r.meldungen.push_back(Fmt(i18n::T("%1 Spiele mit mehr als 5 Spuren: Spurdateien für GDEMU umbenannt (Inhalt unverändert, alte GDI als gdialt.txt)"),
                                      {std::to_string(umbenannt)}));
    }

    std::vector<std::pair<std::wstring, std::string>> kandidaten;
    {
        std::error_code ec2;
        for (const auto& e : fs::directory_iterator(karte / L"DC-RAW_ORIG_BACKUP" / L"01", ec2)) {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
            if (ext == L".gdi") kandidaten.push_back({e.path().wstring(), i18n::T("gesichertem Startmenü")});
        }
    }
    for (int stufe = 0; stufe < 3; ++stufe)
        for (const KartenPlatz& k : plaetze) {

            if (k.format != "GDI" || (k.platz == platz && k.eigenes_menue)) continue;
            const bool passt = stufe == 0 ? (k.platz == 1 && !k.eigenes_menue)
                             : stufe == 1 ? k.eigenes_menue
                                          : (k.platz != 1 && !k.eigenes_menue);
            if (passt) kandidaten.push_back({k.abbild, Fmt(i18n::T("Platz %1"), {std::to_string(k.platz)})});
        }
    std::vector<uint8_t> ip;
    std::string f;
    for (int nur_jue = 1; nur_jue >= 0 && ip.empty(); --nur_jue)
        for (const auto& kd : kandidaten) {
            if (!IpBinAusGdi(kd.first, ip, f) || (nur_jue && std::memcmp(&ip[0x30], "JUE", 3) != 0)) {
                ip.clear();
                continue;
            }
            break;
        }
    if (ip.empty()) { r.fehler = i18n::T("Kein passendes Spiel-Abbild auf der Karte gefunden; zuerst mindestens ein GDI-Spiel auf die Karte legen"); return r; }

    IpBinAnpassen(ip, "1ST_READ.BIN", "DC-RAW", "");
    if (!g_startlogo.empty()) IpLogoEinsetzen(ip, g_startlogo);

    if (ra_status) {

        melde("Schritt 2/5: Spiele bei RetroAchievements prüfen ...");
        const RaPruefung p = RaStatusBestimmen(plaetze, true, nullptr, fortschritt);
        r.meldungen.push_back(Fmt(i18n::T("RA-Kennzeichen aus dem Cache: %1 bekannt, %2 unbekannt, %3 ungeprüft"),
                                  {std::to_string(p.bekannt), std::to_string(p.unbekannt), std::to_string(p.offen)}));
    }
    const std::string liste = SpielelisteText(plaetze, platz);
    for (const KartenPlatz& k : plaetze) if (k.platz != platz && !k.format.empty() && !k.eigenes_menue) ++r.spiele;

    const fs::path arbeit = fs::temp_directory_path(ec) / L"dream-raw-abbild";
    fs::remove_all(arbeit, ec);
    fs::create_directories(arbeit, ec);
    std::vector<GdiDatei> dateien = {{"1ST_READ.BIN", erstlauf}, {"LISTE.TXT", {liste.begin(), liste.end()}}};
    melde("Schritt 3/5: RA-Icons ...");
    if (zusatz)
        for (GdiDatei& d : zusatz(plaetze, r.meldungen)) dateien.push_back(std::move(d));
    melde("Schritt 4/5: erzeuge das Menü-Abbild ...");
    if (!GdiSchreiben(arbeit.wstring(), ip, dateien, r.fehler)) return r;
    const std::string name = "DC-RAW\r\n";
    const std::string marke = "Von DC-RAW geschrieben. Originale liegen in DC-RAW_ORIG_BACKUP.\r\n";
    if (!DateiSchreiben(arbeit / L"name.txt", {name.begin(), name.end()}, r.fehler)) return r;
    if (!DateiSchreiben(arbeit / L"dc-raw.txt", {marke.begin(), marke.end()}, r.fehler)) return r;

    const fs::path ziel = karte / PlatzName(platz);
    if (fs::exists(ziel, ec) && !EigenesMenue(ziel)) {
        const fs::path sicherung = karte / L"DC-RAW_ORIG_BACKUP" / PlatzName(platz);
        if (fs::exists(sicherung, ec) && !fs::is_empty(sicherung, ec)) {

            bool gleich = true;
            size_t n_ziel = 0, n_sich = 0;
            for (const auto& e : fs::directory_iterator(ziel, ec)) {
                ++n_ziel;
                std::vector<uint8_t> a, b;
                if (!DateiLesen(e.path(), a) || !DateiLesen(sicherung / e.path().filename(), b) || a != b) {
                    gleich = false;
                    break;
                }
            }
            for (const auto& e : fs::directory_iterator(sicherung, ec)) { (void)e; ++n_sich; }
            if (!gleich || n_ziel != n_sich) {
                r.fehler = i18n::T("Sicherung für diesen Platz existiert bereits und unterscheidet sich vom Inhalt des "
                             "Platzes, nichts geschrieben");
                return r;
            }
            for (const fs::path& p : Dateiliste(ziel)) fs::remove(p, ec);
            r.meldungen.push_back(Fmt(i18n::T("Platz %1 war bereits identisch gesichert, doppelte Dateien entfernt"),
                                      {std::to_string(platz)}));
        } else {
            fs::create_directories(sicherung, ec);
            for (const fs::path& p : Dateiliste(ziel)) {
                fs::rename(p, sicherung / p.filename(), ec);
                if (ec) { r.fehler = Fmt(i18n::T("Sicherung fehlgeschlagen (%1), nichts geschrieben"), {ec.message()}); return r; }
            }
            r.meldungen.push_back(Fmt(i18n::T("Vorherige Dateien von Platz %1 gesichert in DC-RAW_ORIG_BACKUP"),
                                      {std::to_string(platz)}));
        }
    } else if (fs::exists(ziel, ec)) {
        for (const fs::path& p : Dateiliste(ziel)) fs::remove(p, ec);
    }
    fs::create_directories(ziel, ec);

    std::vector<fs::path> quellen;
    for (const auto& e : fs::directory_iterator(arbeit, ec)) quellen.push_back(e.path());
    size_t nr = 0;
    for (const fs::path& q : quellen) {
        ++nr;
        melde(Fmt(i18n::T("Schritt 5/5: schreibe und prüfe %1 (%2/%3) ..."),
                  {q.filename().string(), std::to_string(nr), std::to_string(quellen.size())}));
        const fs::path z = ziel / q.filename();
        if (!fs::copy_file(q, z, fs::copy_options::overwrite_existing, ec)) {
            r.fehler = Fmt(i18n::T("Kopieren fehlgeschlagen: %1"), {q.filename().string()});
            return r;
        }
        std::vector<uint8_t> a, b;
        if (!DateiLesen(q, a) || !DateiLesen(z, b) || a != b) {
            r.fehler = Fmt(i18n::T("Vergleich nach dem Schreiben fehlgeschlagen: %1"), {q.filename().string()});
            return r;
        }
    }
    fs::remove_all(arbeit, ec);
    r.meldungen.push_back(Fmt(i18n::T("DC-RAW auf Platz %1 geschrieben, %2 Spiele in der Liste, alle Dateien geprüft"),
                              {std::to_string(platz), std::to_string(r.spiele)}));
    r.ok = true;
    return r;
}

bool SicherungVorhanden(const std::wstring& wurzel, int platz) {
    std::error_code ec;
    const fs::path s = fs::path(wurzel) / L"DC-RAW_ORIG_BACKUP" / PlatzName(platz);
    return fs::is_directory(s, ec) && !fs::is_empty(s, ec);
}

int SpurenZuruecksetzen(const fs::path& ordner, std::string& meldung) {
    std::error_code ec;
    const fs::path alt_gdi = ordner / L"gdialt.txt";
    if (!fs::exists(alt_gdi, ec)) return 0;
    std::string name = "disc.gdi";
    {
        std::vector<uint8_t> n;
        if (DateiLesen(ordner / L"gdialt_name.txt", n) && !n.empty()) name = Trim(std::string(n.begin(), n.end()));
    }

    auto lesen = [](const fs::path& p, std::map<int, std::string>& m) {
        std::ifstream f(p);
        if (!f) return false;
        std::string z;
        bool kopf = true;
        while (std::getline(f, z)) {
            if (!z.empty() && z.back() == '\r') z.pop_back();
            if (Trim(z).empty()) continue;
            if (kopf) { kopf = false; continue; }
            std::istringstream is(z);
            int nr = 0;
            std::string lba, typ, sg, datei;
            is >> nr >> lba >> typ >> sg >> std::ws;
            if (is.peek() == '"') { is.get(); std::getline(is, datei, '"'); }
            else is >> datei;
            if (!is && datei.empty()) return false;
            m[nr] = datei;
        }
        return !m.empty();
    };
    std::map<int, std::string> alt, jetzt;
    if (!lesen(alt_gdi, alt) || !lesen(ordner / L"disc.gdi", jetzt) || alt.size() != jetzt.size()) {
        meldung = i18n::T("GDI nicht lesbar");
        return -1;
    }
    for (const auto& e : jetzt)
        if (!alt.count(e.first) || !fs::exists(ordner / fs::u8path(e.second), ec)) { meldung = i18n::T("GDI nicht lesbar"); return -1; }

    for (const auto& e : jetzt) {
        if (e.second == alt[e.first]) continue;
        fs::rename(ordner / fs::u8path(e.second), ordner / (L"_zu_" + std::to_wstring(e.first) + L".tmp"), ec);
        if (ec) { meldung = "rename"; return -1; }
    }
    for (const auto& e : jetzt) {
        if (e.second == alt[e.first]) continue;
        fs::rename(ordner / (L"_zu_" + std::to_wstring(e.first) + L".tmp"), ordner / fs::u8path(alt[e.first]), ec);
        if (ec) { meldung = "rename"; return -1; }
    }

    std::vector<uint8_t> inhalt;
    if (!DateiLesen(alt_gdi, inhalt)) { meldung = i18n::T("GDI nicht lesbar"); return -1; }
    std::string fehler;
    if (name != "disc.gdi") fs::remove(ordner / L"disc.gdi", ec);
    if (!DateiSchreiben(ordner / fs::u8path(name), inhalt, fehler)) { meldung = fehler; return -1; }
    fs::remove(alt_gdi, ec);
    fs::remove(ordner / L"gdialt_name.txt", ec);
    return 1;
}

MenueErgebnis OriginalWiederherstellen(const std::wstring& wurzel, int platz,
                                       const std::function<void(const std::string&)>& fortschritt) {
    auto melde = [&](const std::string& s) { if (fortschritt) fortschritt(s); };
    MenueErgebnis r;
    std::error_code ec;
    const fs::path karte(wurzel);
    const fs::path sicherung = karte / L"DC-RAW_ORIG_BACKUP" / PlatzName(platz);
    const fs::path ziel = karte / PlatzName(platz);
    if (!SicherungVorhanden(wurzel, platz)) {
        r.fehler = Fmt(i18n::T("Für Platz %1 gibt es keine Sicherung"), {std::to_string(platz)});
        return r;
    }

    if (fs::exists(ziel, ec) && !fs::is_empty(ziel, ec) && !EigenesMenue(ziel)) {
        bool teil = true;
        for (const fs::path& p : Dateiliste(ziel))
            if (!fs::exists(sicherung / p.filename(), ec)) { teil = false; break; }
        if (!teil) {
            r.fehler = Fmt(i18n::T("Auf Platz %1 liegt nicht DC-RAW, nichts geändert"), {std::to_string(platz)});
            return r;
        }
    }

    const std::vector<fs::path> quellen = Dateiliste(sicherung);
    for (const fs::path& q : quellen)
        if (!fs::is_regular_file(q, ec)) { r.fehler = Fmt(i18n::T("Sicherung unvollständig: %1"), {q.filename().string()}); return r; }
    for (const fs::path& p : Dateiliste(ziel)) {
        fs::remove(p, ec);
        if (ec) { r.fehler = Fmt(i18n::T("Konnte %1 nicht entfernen, Sicherung unverändert"), {p.filename().string()}); return r; }
    }
    fs::create_directories(ziel, ec);
    size_t nr = 0;
    for (const fs::path& q : quellen) {
        ++nr;
        melde(Fmt(i18n::T("Stelle wieder her: %1 (%2/%3) ..."),
                  {q.filename().string(), std::to_string(nr), std::to_string(quellen.size())}));
        const fs::path z = ziel / q.filename();
        std::vector<uint8_t> a, b;
        if (!fs::copy_file(q, z, fs::copy_options::overwrite_existing, ec) || !DateiLesen(q, a) || !DateiLesen(z, b) ||
            a != b) {
            r.fehler = Fmt(i18n::T("Zurückspielen fehlgeschlagen: %1 – die Sicherung bleibt erhalten"), {q.filename().string()});
            return r;
        }
    }

    for (const fs::path& q : quellen) fs::remove(q, ec);
    fs::remove(sicherung, ec);
    fs::remove(karte / L"DC-RAW_ORIG_BACKUP", ec);
    r.meldungen.push_back(Fmt(i18n::T("Ursprüngliches Menü auf Platz %1 wiederhergestellt, %2 Dateien geprüft"),
                              {std::to_string(platz), std::to_string(quellen.size())}));

    {
        int zurueck = 0, fehlgeschlagen = 0;
        for (const auto& e : fs::directory_iterator(karte, ec)) {
            if (!e.is_directory(ec)) continue;
            const std::wstring n = e.path().filename().wstring();
            if (n.empty() || n.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
            std::string m;
            const int x = SpurenZuruecksetzen(e.path(), m);
            if (x > 0) ++zurueck;
            else if (x < 0) ++fehlgeschlagen;
        }
        if (zurueck) r.meldungen.push_back(Fmt(i18n::T("Spurdateien von %1 Spielen auf die ursprünglichen Namen zurückgesetzt"), {std::to_string(zurueck)}));
        if (fehlgeschlagen) r.meldungen.push_back(Fmt(i18n::T("Bei %1 Spielen konnten die Spurdateien nicht zurückgesetzt werden"), {std::to_string(fehlgeschlagen)}));
    }
    r.ok = true;
    return r;
}

}
