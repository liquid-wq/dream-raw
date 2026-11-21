// Translations.

#include "i18n.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace i18n {
namespace {

struct Eintrag { const char* t[kSprachen]; };

const Eintrag kTabelle[] = {
#include "i18n_tabelle.inc"
};

std::atomic<int> g_index{1};
std::string g_sprache = "en";
std::mutex g_mutex;

const std::unordered_map<std::string, const Eintrag*>& Index() {
    static const std::unordered_map<std::string, const Eintrag*> m = []() {
        std::unordered_map<std::string, const Eintrag*> x;
        for (const Eintrag& e : kTabelle) x.emplace(e.t[0], &e);
        return x;
    }();
    return m;
}

std::mutex g_mitid_mutex;
std::unordered_map<std::string, std::array<std::string, kSprachen>> g_mitid;

const char* Spalte(const Eintrag* e, int i) {
    if (e->t[i] && *e->t[i]) return e->t[i];
    if (e->t[1] && *e->t[1]) return e->t[1];
    return e->t[0];
}

}

const std::vector<SpracheInfo>& Sprachen() {
    static const std::vector<SpracheInfo> s = {
        {"de", "Deutsch"},
        {"en", "English"},
        {"es", "Espa\xC3\xB1ol"},
        {"fr", "Fran\xC3\xA7" "ais"},
        {"it", "Italiano"},
        {"pl", "Polski"},
        {"pt", "Portugu\xC3\xAAs"},
    };
    return s;
}

void SpracheSetzen(const std::string& kennung) {
    const auto& s = Sprachen();
    for (size_t i = 0; i < s.size(); ++i) {
        if (kennung == s[i].kennung) {
            std::lock_guard<std::mutex> l(g_mutex);
            g_sprache = kennung;
            g_index = static_cast<int>(i);
            return;
        }
    }
}

const std::string& Sprache() { return g_sprache; }
int SpracheIndex() { return g_index.load(); }
bool Englisch() { return g_index.load() != 0; }

std::string SystemSprache() {
    switch (PRIMARYLANGID(::GetUserDefaultUILanguage())) {
        case LANG_GERMAN:     return "de";
        case LANG_SPANISH:    return "es";
        case LANG_FRENCH:     return "fr";
        case LANG_ITALIAN:    return "it";
        case LANG_POLISH:     return "pl";
        case LANG_PORTUGUESE: return "pt";
        default:              return "en";
    }
}

const char* T(const char* deutsch) {
    if (!deutsch || !*deutsch) return deutsch;
    const int i = g_index.load();
    const char* raute = std::strstr(deutsch, "##");
    if (!raute) {
        const auto& m = Index();
        const auto it = m.find(deutsch);
        return it == m.end() ? deutsch : Spalte(it->second, i);
    }

    const std::string text(deutsch, raute - deutsch);
    const auto& m = Index();
    const auto it = m.find(text);
    if (it == m.end()) return deutsch;
    std::lock_guard<std::mutex> l(g_mitid_mutex);
    auto& zeile = g_mitid[deutsch];
    if (zeile[i].empty()) zeile[i] = std::string(Spalte(it->second, i)) + raute;
    return zeile[i].c_str();
}

std::string T(const std::string& deutsch) { return T(deutsch.c_str()); }

std::string Fmt(const std::string& muster, std::initializer_list<std::string> werte) {
    std::string aus;
    aus.reserve(muster.size() + 32);
    const std::string* w = werte.begin();
    const size_t n = werte.size();
    for (size_t i = 0; i < muster.size(); ++i) {
        if (muster[i] == '%' && i + 1 < muster.size() && muster[i + 1] >= '1' && muster[i + 1] <= '9') {
            const size_t k = static_cast<size_t>(muster[i + 1] - '1');
            if (k < n) { aus += w[k]; ++i; continue; }
        }
        aus.push_back(muster[i]);
    }
    return aus;
}

}
