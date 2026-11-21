// Memory ranges an achievement set needs.

#include "core/adressliste.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "rc_runtime.h"

namespace fs = std::filesystem;

namespace core {
namespace {

constexpr uint32_t kRamGroesse = 0x1000000;

uint32_t RC_CCONV Lesen(uint32_t adresse, uint32_t anzahl, void* ud) {
    auto* menge = static_cast<std::set<uint32_t>*>(ud);
    for (uint32_t i = 0; i < anzahl; ++i)
        if (adresse + i < kRamGroesse) menge->insert(adresse + i);
    return 0;
}

void RC_CCONV Ereignis(const rc_runtime_event_t*) {}

fs::path Ordner() {
    const wchar_t* a = _wgetenv(L"APPDATA");
    if (!a) return {};
    const fs::path o = fs::path(a) / L"DREAM-RAW" / L"adresslisten";
    std::error_code ec;
    fs::create_directories(o, ec);
    return o;
}

}

std::vector<Bereich> AdresslisteAusSpiel(const SpielDaten& s, size_t& adressen) {
    rc_runtime_t rt;
    rc_runtime_init(&rt);
    for (const Achievement& a : s.achievements)
        if (!a.definition.empty()) rc_runtime_activate_achievement(&rt, a.id, a.definition.c_str(), nullptr, 0);
    for (const auto& l : s.leaderboards) rc_runtime_activate_lboard(&rt, l.first, l.second.c_str(), nullptr, 0);
    if (!s.rich_presence.empty()) rc_runtime_activate_richpresence(&rt, s.rich_presence.c_str(), nullptr, 0);
    std::set<uint32_t> menge;
    rc_runtime_do_frame(&rt, Ereignis, Lesen, &menge, nullptr);
    rc_runtime_destroy(&rt);
    adressen = menge.size();
    std::vector<Bereich> b;
    BereicheBilden(menge, b);
    return b;
}

bool AdresslisteSpeichern(uint32_t spiel_id, const std::vector<Bereich>& b) {
    const fs::path o = Ordner();
    if (o.empty()) return false;
    std::ofstream f(o / (std::to_wstring(spiel_id) + L".txt"));
    for (const Bereich& x : b) f << x.adresse << ' ' << x.laenge << '\n';
    return static_cast<bool>(f);
}

bool AdresslisteLaden(uint32_t spiel_id, std::vector<Bereich>& b) {
    b.clear();
    const fs::path o = Ordner();
    if (o.empty()) return false;
    std::ifstream f(o / (std::to_wstring(spiel_id) + L".txt"));
    if (!f) return false;
    uint32_t a = 0, l = 0;
    while (f >> a >> l) b.push_back({a, static_cast<uint16_t>(l)});
    return !b.empty();
}

std::vector<uint8_t> AdressenBinBauen(const std::vector<std::pair<int, std::vector<Bereich>>>& listen) {
    std::vector<uint8_t> d = {'D', 'R', 'A', 'D'};
    auto u16 = [&](uint32_t v) { d.push_back(static_cast<uint8_t>(v)); d.push_back(static_cast<uint8_t>(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
    u16(static_cast<uint32_t>(listen.size()));
    for (const auto& e : listen) {
        u16(static_cast<uint32_t>(e.first));
        u16(static_cast<uint32_t>(e.second.size()));
        for (const Bereich& b : e.second) { u32(b.adresse); u16(b.laenge); }
    }
    return d;
}

}
