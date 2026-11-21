// Local cache of game identifications.

#include "core/hashcache.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>

namespace core {

HashCache& HashCache::Global() {
    static HashCache c;
    if (c.pfad_.empty()) {
        wchar_t* appdata = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
            std::wstring ordner = std::wstring(appdata) + L"\\DREAM-RAW";
            CreateDirectoryW(ordner.c_str(), nullptr);
            c.pfad_ = ordner + L"\\hash_cache.txt";
        }
        CoTaskMemFree(appdata);
    }
    return c;
}

void HashCache::Laden() {
    geladen_ = true;
    if (pfad_.empty()) return;
    FILE* f = _wfopen(pfad_.c_str(), L"r");
    if (!f) return;
    char hash[64];
    unsigned long id;
    while (std::fscanf(f, "%63s %lu", hash, &id) == 2)
        if (std::strlen(hash) == 32) eintraege_[hash] = static_cast<uint32_t>(id);
    std::fclose(f);
}

void HashCache::Speichern() const {
    if (pfad_.empty()) return;
    const std::wstring tmp = pfad_ + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"w");
    if (!f) return;
    bool ok = true;
    for (const auto& e : eintraege_)
        ok = ok && std::fprintf(f, "%s %lu\n", e.first.c_str(), static_cast<unsigned long>(e.second)) > 0;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) return;
    MoveFileExW(tmp.c_str(), pfad_.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool HashCache::Nachsehen(const std::string& hash, uint32_t& spiel_id) {
    std::lock_guard<std::mutex> l(sperre_);
    if (!geladen_) Laden();
    auto it = eintraege_.find(hash);
    if (it == eintraege_.end()) return false;
    spiel_id = it->second;
    return true;
}

void HashCache::Eintragen(const std::map<std::string, uint32_t>& neu) {
    if (neu.empty()) return;
    std::lock_guard<std::mutex> l(sperre_);
    if (!geladen_) Laden();

    for (const auto& e : neu) {
        auto it = eintraege_.find(e.first);
        if (e.second == 0 && it != eintraege_.end() && it->second != 0) continue;
        eintraege_[e.first] = e.second;
    }
    Speichern();
}

HashAufloesung HashCache::Aufloesen(const std::string& hash, bool unbekannt_neu_pruefen) {
    {
        std::lock_guard<std::mutex> l(sperre_);
        if (!geladen_) Laden();
        auto it = eintraege_.find(hash);
        if (it != eintraege_.end() && !(unbekannt_neu_pruefen && it->second == 0)) {
            HashAufloesung a;
            a.ok = true;
            a.spiel_id = it->second;
            return a;
        }
    }
    HashAufloesung a = HashAufloesen(hash);
    if (a.ok) {
        std::lock_guard<std::mutex> l(sperre_);
        eintraege_[hash] = a.spiel_id;
        Speichern();
    }
    return a;
}

}
