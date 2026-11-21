// RetroAchievements requests.

#include "core/ra.h"

#include <cctype>

#include <cstdio>
#include <cstring>

#include "core/http.h"

extern "C" {
#include "rc_api_info.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_error.h"
#include "rc_hash.h"
}

namespace core {
namespace {

bool Senden(const rc_api_request_t& anfrage, std::string& koerper,
            rc_api_server_response_t& antwort, std::string& fehler) {
    const HttpAntwort a = HttpAnfrage(anfrage.url, anfrage.post_data ? anfrage.post_data : "");

    if (a.status == 0) { fehler = a.fehler; return false; }
    koerper = a.koerper;
    antwort.body = koerper.c_str();
    antwort.body_length = koerper.size();
    antwort.http_status_code = a.status;
    return true;
}

std::string RcFehler(int code, const rc_api_response_t& r) {
    if (r.error_message && *r.error_message) return r.error_message;
    return rc_error_str(code);
}

void HashMeldung(const char* text, const rc_hash_iterator_t* it) {
    auto* ziel = static_cast<std::string*>(it->userdata);
    if (!ziel || !text) return;
    *ziel += text;
    *ziel += '\n';
}

rc_hash_cdreader_t g_standard_leser;
const char* g_patch_bereich = nullptr;
int g_patch_vga = -1;
IpFelder* g_original = nullptr;

size_t RC_CCONV SektorLesenMitPatch(void* spur, uint32_t sektor, void* puffer, size_t anzahl) {
    const size_t n = g_standard_leser.read_sector(spur, sektor, puffer, anzahl);
    auto* p = static_cast<uint8_t*>(puffer);
    if (n >= 256 && std::memcmp(p, "SEGA SEGAKATANA ", 16) == 0) {
        if (g_original && !g_original->ok) {
            g_original->ok = true;
            g_original->geraet.assign(reinterpret_cast<char*>(p + 0x20), 16);
            g_original->bereich.assign(reinterpret_cast<char*>(p + 0x30), 8);
            g_original->peripherie.assign(reinterpret_cast<char*>(p + 0x38), 8);
        }
        if (g_patch_bereich) std::memcpy(p + 0x30, g_patch_bereich, 8);
        if (g_patch_vga >= 0) {

            char c = static_cast<char>(p[0x3D]);
            int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (v >= 0) {
                v = g_patch_vga ? (v | 1) : (v & ~1);
                p[0x3D] = static_cast<uint8_t>("0123456789ABCDEF"[v]);
            }
        }
    }
    return n;
}

}

HashErgebnis ImageHashenMitPatch(const std::string& pfad_utf8, const char* bereich8, int vga,
                                 IpFelder* original) {
    HashErgebnis erg;
    rc_hash_iterator_t it;
    rc_hash_initialize_iterator(&it, pfad_utf8.c_str(), nullptr, 0);
    it.userdata = &erg.meldungen;
    it.callbacks.error_message = HashMeldung;
    rc_hash_get_default_cdreader(&g_standard_leser);
    it.callbacks.cdreader = g_standard_leser;
    it.callbacks.cdreader.read_sector = SektorLesenMitPatch;
    g_patch_bereich = bereich8;
    g_patch_vga = vga;
    g_original = original;

    char hash[33] = {};
    erg.ok = rc_hash_generate(hash, kKonsoleDreamcast, &it) != 0;
    rc_hash_destroy_iterator(&it);
    g_patch_bereich = nullptr;
    g_patch_vga = -1;
    g_original = nullptr;
    if (erg.ok) erg.hash = hash;
    else erg.fehler = "Hash konnte nicht erzeugt werden";
    return erg;
}

namespace {
Anmeldung Login2(const std::string& benutzer, const std::string& passwort, const std::string& token);
}

Anmeldung Anmelden(const std::string& benutzer, const std::string& passwort) {
    return Login2(benutzer, passwort, "");
}

Anmeldung AnmeldenMitToken(const std::string& benutzer, const std::string& token) {
    return Login2(benutzer, "", token);
}

namespace {
Anmeldung Login2(const std::string& benutzer, const std::string& passwort, const std::string& token) {
    Anmeldung erg;
    rc_api_login_request_t p = {};
    p.username = benutzer.c_str();
    if (!passwort.empty()) p.password = passwort.c_str();
    if (!token.empty()) p.api_token = token.c_str();

    rc_api_request_t anfrage;
    int rc = rc_api_init_login_request(&anfrage, &p);
    if (rc != RC_OK) { erg.fehler = rc_error_str(rc); return erg; }

    std::string koerper;
    rc_api_server_response_t sa = {};
    const bool gesendet = Senden(anfrage, koerper, sa, erg.fehler);
    rc_api_destroy_request(&anfrage);
    if (!gesendet) return erg;

    rc_api_login_response_t r = {};
    rc = rc_api_process_login_server_response(&r, &sa);
    if (rc == RC_OK && r.response.succeeded) {
        erg.ok = true;
        erg.benutzer = r.username ? r.username : benutzer;
        erg.token = r.api_token ? r.api_token : "";
        erg.punkte = r.score;
        erg.punkte_softcore = r.score_softcore;
    } else {
        erg.fehler = RcFehler(rc, r.response);
    }
    rc_api_destroy_login_response(&r);
    return erg;
}
}

HashErgebnis ImageHashen(const std::string& pfad_utf8) {
    HashErgebnis erg;
    rc_hash_iterator_t it;
    rc_hash_initialize_iterator(&it, pfad_utf8.c_str(), nullptr, 0);
    it.userdata = &erg.meldungen;
    it.callbacks.error_message = HashMeldung;
    it.callbacks.verbose_message = HashMeldung;

    char hash[33] = {};
    erg.ok = rc_hash_generate(hash, kKonsoleDreamcast, &it) != 0;
    rc_hash_destroy_iterator(&it);

    if (erg.ok) {
        erg.hash = hash;
    } else {

        std::string m = erg.meldungen;
        while (!m.empty() && m.back() == '\n') m.pop_back();
        const size_t nl = m.rfind('\n');
        erg.fehler = m.empty() ? "Hash konnte nicht erzeugt werden"
                               : (nl == std::string::npos ? m : m.substr(nl + 1));
    }
    return erg;
}

HashAufloesung HashAufloesen(const std::string& hash) {
    HashAufloesung erg;
    rc_api_resolve_hash_request_t p = {};
    p.game_hash = hash.c_str();

    rc_api_request_t anfrage;
    int rc = rc_api_init_resolve_hash_request(&anfrage, &p);
    if (rc != RC_OK) { erg.fehler = rc_error_str(rc); return erg; }

    std::string koerper;
    rc_api_server_response_t sa = {};
    const bool gesendet = Senden(anfrage, koerper, sa, erg.fehler);
    rc_api_destroy_request(&anfrage);
    if (!gesendet) return erg;

    rc_api_resolve_hash_response_t r = {};
    rc = rc_api_process_resolve_hash_server_response(&r, &sa);
    if (rc == RC_OK && r.response.succeeded) {
        erg.ok = true;
        erg.spiel_id = r.game_id;
    } else {
        erg.fehler = RcFehler(rc, r.response);
    }
    rc_api_destroy_resolve_hash_response(&r);
    return erg;
}

HashBibliothek HashBibliothekLaden(uint32_t konsole) {
    HashBibliothek erg;
    rc_api_fetch_hash_library_request_t p = {};
    p.console_id = konsole;
    rc_api_request_t anfrage;
    int rc = rc_api_init_fetch_hash_library_request(&anfrage, &p);
    if (rc != RC_OK) { erg.fehler = rc_error_str(rc); return erg; }
    std::string koerper;
    rc_api_server_response_t sa = {};
    const bool gesendet = Senden(anfrage, koerper, sa, erg.fehler);
    rc_api_destroy_request(&anfrage);
    if (!gesendet) return erg;
    rc_api_fetch_hash_library_response_t r = {};
    rc = rc_api_process_fetch_hash_library_server_response(&r, &sa);
    if (rc == RC_OK && r.response.succeeded) {
        erg.ok = true;
        for (uint32_t i = 0; i < r.num_entries; ++i)
            if (r.entries[i].hash) {
                std::string h = r.entries[i].hash;
                for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                erg.hashes[h] = r.entries[i].game_id;
            }
    } else {
        erg.fehler = RcFehler(rc, r.response);
    }
    rc_api_destroy_fetch_hash_library_response(&r);
    return erg;
}

SpielDaten SpielLaden(const std::string& benutzer, const std::string& token, uint32_t spiel_id) {
    SpielDaten erg;
    rc_api_fetch_game_data_request_t p = {};
    p.username = benutzer.c_str();
    p.api_token = token.c_str();
    p.game_id = spiel_id;

    rc_api_request_t anfrage;
    int rc = rc_api_init_fetch_game_data_request(&anfrage, &p);
    if (rc != RC_OK) { erg.fehler = rc_error_str(rc); return erg; }

    std::string koerper;
    rc_api_server_response_t sa = {};
    const bool gesendet = Senden(anfrage, koerper, sa, erg.fehler);
    rc_api_destroy_request(&anfrage);
    if (!gesendet) return erg;

    rc_api_fetch_game_data_response_t r = {};
    rc = rc_api_process_fetch_game_data_server_response(&r, &sa);
    if (rc == RC_OK && r.response.succeeded) {
        erg.ok = true;
        erg.id = r.id;
        erg.konsole = r.console_id;
        erg.titel = r.title ? r.title : "";
        erg.icon_url = r.image_url ? r.image_url : "";
        for (uint32_t i = 0; i < r.num_achievements; ++i) {
            const rc_api_achievement_definition_t& d = r.achievements[i];
            Achievement a;
            a.id = d.id;
            a.punkte = d.points;
            a.offiziell = d.category == RC_ACHIEVEMENT_CATEGORY_CORE;
            a.titel = d.title ? d.title : "";
            a.beschreibung = d.description ? d.description : "";
            a.definition = d.definition ? d.definition : "";
            erg.achievements.push_back(a);
        }
        for (uint32_t i = 0; i < r.num_leaderboards; ++i)
            if (r.leaderboards[i].definition)
                erg.leaderboards.emplace_back(r.leaderboards[i].id, r.leaderboards[i].definition);
        erg.rich_presence = r.rich_presence_script ? r.rich_presence_script : "";
    } else {
        erg.fehler = RcFehler(rc, r.response);
    }
    rc_api_destroy_fetch_game_data_response(&r);
    return erg;
}

}
