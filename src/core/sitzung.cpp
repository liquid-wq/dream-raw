// Session: cable, game detection and achievement evaluation.

#include "core/sitzung.h"

#include "core/text.h"
#include "i18n.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <deque>
#include <mutex>
#include <set>
#include <vector>

#include "core/adressliste.h"
#include "core/dcprotokoll.h"
#include "core/hashcache.h"
#include "core/http.h"
#include "core/ra.h"
#include "core/seriell.h"

extern "C" {
#include "rc_client.h"
#include "rc_error.h"
}

namespace core {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kBaud = 1562500;
constexpr uint32_t kRamGroesse = 0x1000000;
constexpr uint8_t kAbzugListe = 0xFD;

using Uhr = std::chrono::steady_clock;

std::string Hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%06X", v);
    return b;
}

long long Ms(Uhr::time_point seit) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Uhr::now() - seit).count();
}

struct HttpAuftrag {
    std::string url, formular;
    rc_client_server_callback_t rueckruf = nullptr;
    void* rueckruf_daten = nullptr;
};
struct HttpErgebnis {
    HttpAuftrag auftrag;
    HttpAntwort antwort;
};

class HttpArbeiter {
public:
    HttpArbeiter() : thread_([this] { Laufen(); }) {}
    ~HttpArbeiter() {
        { std::lock_guard<std::mutex> l(m_); stopp_ = true; }
        cv_.notify_all();
        thread_.join();
    }
    void Auftrag(HttpAuftrag a) {
        { std::lock_guard<std::mutex> l(m_); auftraege_.push_back(std::move(a)); }
        cv_.notify_all();
    }
    bool Fertig(HttpErgebnis& e) {
        std::lock_guard<std::mutex> l(m_);
        if (fertig_.empty()) return false;
        e = std::move(fertig_.front());
        fertig_.pop_front();
        return true;
    }

private:
    void Laufen() {
        for (;;) {
            HttpAuftrag a;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stopp_ || !auftraege_.empty(); });
                if (stopp_) return;
                a = std::move(auftraege_.front());
                auftraege_.pop_front();
            }
            HttpErgebnis e;
            e.antwort = HttpAnfrage(a.url, a.formular);
            e.auftrag = std::move(a);
            std::lock_guard<std::mutex> l(m_);
            fertig_.push_back(std::move(e));
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<HttpAuftrag> auftraege_;
    std::deque<HttpErgebnis> fertig_;
    bool stopp_ = false;
    std::thread thread_;
};

constexpr size_t kKtMuster = 64;
uint8_t KtMuster(size_t i) {
    static const uint8_t anfang[4] = {0x00, 0xFF, 0x55, 0xAA};
    return i < 4 ? anfang[i] : static_cast<uint8_t>(i * 37 + 5);
}

struct Kontext {
    Sitzung::Melder melder;
    HttpArbeiter http;
    rc_client_t* client = nullptr;

    std::vector<uint8_t> spiegel = std::vector<uint8_t>(kRamGroesse, 0);
    std::vector<bool> empfangen = std::vector<bool>(kRamGroesse, false);
    std::set<uint32_t> adressen;
    std::vector<uint8_t> vorher;
    bool adressen_neu = false;
    bool fehlende_gelesen = false;

    std::vector<Bereich> bereiche;
    uint8_t liste_id = 0;
    std::map<uint8_t, std::vector<Bereich>> listen;
    std::map<uint8_t, std::vector<uint8_t>> staende;
    bool liste_bestaetigt = true;
    bool grenze_gemeldet = false;

    bool angemeldet = false;
    bool spiel_geladen = false;
    std::string erster_hash;
    std::string titel;
    std::vector<Bereich> vorgabe;
    std::string hash;
    std::string geladener_hash;

    void Melden(SitzungsMeldung::Art art, const std::string& text) {
        SitzungsMeldung m;
        m.art = art;
        m.text = text;
        melder(m);
    }
};

Kontext* K(const rc_client_t* c) { return static_cast<Kontext*>(rc_client_get_userdata(c)); }

uint32_t RC_CCONV SpeicherLesen(uint32_t adresse, uint8_t* puffer, uint32_t anzahl, rc_client_t* c) {
    Kontext* k = K(c);
    for (uint32_t i = 0; i < anzahl; ++i) {
        const uint32_t a = adresse + i;
        if (a >= kRamGroesse) return i;
        if (k->adressen.insert(a).second) k->adressen_neu = true;
        if (!k->empfangen[a]) k->fehlende_gelesen = true;
        puffer[i] = k->spiegel[a];
    }
    return anzahl;
}

void RC_CCONV ServerAufruf(const rc_api_request_t* anfrage, rc_client_server_callback_t rueckruf,
                           void* rueckruf_daten, rc_client_t* c) {
    HttpAuftrag a;
    a.url = anfrage->url ? anfrage->url : "";
    a.formular = anfrage->post_data ? anfrage->post_data : "";
    a.rueckruf = rueckruf;
    a.rueckruf_daten = rueckruf_daten;
    K(c)->http.Auftrag(std::move(a));
}

void RC_CCONV Ereignis(const rc_client_event_t* e, rc_client_t* c) {
    Kontext* k = K(c);
    switch (e->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
            SitzungsMeldung m;
            m.art = SitzungsMeldung::Art::Freigeschaltet;
            m.achievement_id = e->achievement->id;
            m.punkte = e->achievement->points;
            m.titel = e->achievement->title ? e->achievement->title : "";
            m.beschreibung = e->achievement->description ? e->achievement->description : "";
            m.badge = e->achievement->badge_name;
            m.text = Fmt(T("Freigeschaltet: %1 (%2 Punkte)"), {m.titel, std::to_string(m.punkte)});
            k->melder(m);
            break;
        }
        case RC_CLIENT_EVENT_GAME_COMPLETED:
            k->Melden(SitzungsMeldung::Art::Gut, "Alle Achievements des Spiels erreicht.");
            break;
        case RC_CLIENT_EVENT_SERVER_ERROR:
            k->Melden(SitzungsMeldung::Art::Fehler,
                      Fmt(T("RA-Server: %1"), {e->server_error && e->server_error->error_message
                                                    ? std::string(e->server_error->error_message)
                                                    : std::string(T("Fehler"))}));
            break;
        case RC_CLIENT_EVENT_DISCONNECTED:
            k->Melden(SitzungsMeldung::Art::Warnung, "Freischaltung konnte nicht gemeldet werden, wird wiederholt.");
            break;
        case RC_CLIENT_EVENT_RECONNECTED:
            k->Melden(SitzungsMeldung::Art::Gut, "Ausstehende Freischaltungen gemeldet.");
            break;
        default:
            break;
    }
}

void RC_CCONV RcLog(const char* text, const rc_client_t* c) {
    (void)text; (void)c;
}

void SpielLadenWennBereit(Kontext& k);

void RC_CCONV NachSpielLaden(int ergebnis, const char* fehler, rc_client_t* c, void*) {
    Kontext* k = K(c);
    if (ergebnis != RC_OK) {
        k->Melden(SitzungsMeldung::Art::Fehler,
                  Fmt(T("Spiel laden fehlgeschlagen: %1"), {fehler ? fehler : rc_error_str(ergebnis)}));
        return;
    }
    const rc_client_game_t* g = rc_client_get_game_info(c);
    k->spiel_geladen = true;

    k->vorgabe.clear();

    if (g && g->id && AdresslisteLaden(g->id, k->vorgabe)) {
    } else {
        k->Melden(SitzungsMeldung::Art::Warnung,
                  "Für dieses Spiel fehlen Daten auf der Karte – Karte mit DREAM-RAW neu einrichten (angemeldet).");
    }
    SitzungsMeldung m;
    m.art = SitzungsMeldung::Art::Spiel;
    m.hash = k->geladener_hash;
    m.spiel_id = g ? g->id : 0;
    m.titel = g && g->title ? g->title : "";
    m.text = Fmt(T("Spiel geladen: %1 (ID %2)"), {m.titel, std::to_string(m.spiel_id)});
    if (m.spiel_id == 0) {
        m.text = Fmt(T("Spiel ist bei RetroAchievements unbekannt (Hash %1)"), {m.hash});
        k->spiel_geladen = false;
    } else {

        rc_client_achievement_list_t* l = rc_client_create_achievement_list(
            c, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
        if (l) {
            for (uint32_t b = 0; b < l->num_buckets; ++b)
                for (uint32_t i = 0; i < l->buckets[b].num_achievements; ++i) {
                    const rc_client_achievement_t* a = l->buckets[b].achievements[i];
                    if (a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE) m.bereits.push_back(a->id);
                }
            rc_client_destroy_achievement_list(l);
        }
        m.text += Fmt(T(", %1 bereits freigeschaltet"), {std::to_string(m.bereits.size())});
    }
    k->melder(m);
}

void RC_CCONV NachAnmeldung(int ergebnis, const char* fehler, rc_client_t* c, void*) {
    Kontext* k = K(c);
    if (ergebnis != RC_OK) {
        k->Melden(SitzungsMeldung::Art::Fehler,
                  Fmt(T("Anmeldung der Sitzung fehlgeschlagen: %1"), {fehler ? fehler : rc_error_str(ergebnis)}));
        return;
    }
    k->angemeldet = true;
    k->Melden(SitzungsMeldung::Art::Log, "Sitzung bei RetroAchievements angemeldet.");
    SpielLadenWennBereit(*k);
}

void SpielLadenWennBereit(Kontext& k) {
    if (!k.angemeldet || k.hash.empty() || k.hash == k.geladener_hash) return;
    if (k.spiel_geladen) rc_client_unload_game(k.client);
    k.spiel_geladen = false;
    k.geladener_hash = k.hash;

    k.adressen.clear();
    k.bereiche.clear();
    k.listen.clear();
    k.staende.clear();
    k.empfangen.assign(kRamGroesse, false);
    k.vorher.clear();
    k.adressen_neu = false;
    rc_client_begin_load_game(k.client, k.hash.c_str(), NachSpielLaden, nullptr);
}

}

Sitzung::Sitzung(Melder melder) : melder_(std::move(melder)) {}

Sitzung::~Sitzung() { Beenden(); }

void Sitzung::Starten(const std::string& port, const std::string& benutzer, const std::string& token) {
    Beenden();
    stopp_ = false;
    laeuft_ = true;
    thread_ = std::thread([this, port, benutzer, token] { Schleife(port, benutzer, token); });
}

void Sitzung::Beenden() {
    stopp_ = true;
    if (thread_.joinable()) thread_.join();
    laeuft_ = false;
}

void Sitzung::Schleife(std::string port_name, std::string benutzer, std::string token) {
    Kontext k;
    k.melder = melder_;
    k.client = rc_client_create(SpeicherLesen, ServerAufruf);
    rc_client_set_userdata(k.client, &k);
    rc_client_set_event_handler(k.client, Ereignis);
    rc_client_enable_logging(k.client, RC_CLIENT_LOG_LEVEL_WARN, RcLog);
    rc_client_set_hardcore_enabled(k.client, 0);
    rc_client_begin_login_with_token(k.client, benutzer.c_str(), token.c_str(), NachAnmeldung, nullptr);

    SeriellerPort port;
    Paketleser leser;
    std::vector<uint8_t> puffer(65536);
    auto letzter_versuch = Uhr::now() - std::chrono::seconds(10);
    auto letzte_hashanfrage = Uhr::now() - std::chrono::seconds(10);
    auto letzte_liste = Uhr::now();
    auto letzte_statistik = Uhr::now();
    auto letzte_ramanzeige = Uhr::now();

    auto letztes_paket = Uhr::now() - std::chrono::hours(1);
    auto letzte_cts_abfrage = Uhr::now();
    auto cts_seit = Uhr::now();
    int cts_zustand = -1;
    bool cts_im_spiel = false;
    auto letzte_kennung = Uhr::now() - std::chrono::hours(1);
    struct { bool aktiv = false; } abzug;
    bool kt_aktiv = false;
    auto kt_start = Uhr::now();
    unsigned kt_muster_ok = 0, kt_muster_n = 0, kt_rts_n = 0;
    int kt_cts[2] = {-1, -1};
    uint32_t diag_w[Paketleser::kDiagWorte] = {};
    bool diag_da = false;
    auto letzte_diag_zeile = Uhr::now() - std::chrono::hours(1);
    uint32_t br_vorher[3] = {};
    bool br_da = false;
    float spiel_bilder_s = -1, rechenanteil = -1;
    auto letzte_bildrate_zeile = Uhr::now() - std::chrono::hours(1);
    int zuletzt_gemeldete_liste = -1;

    int listen_versuche = 0, versuche_fuer = -1;

    uint32_t z_an = 0, z_fremd = 0, z_voll = 0, z_ohne = 0, z_aus = 0;
    int64_t z_liste_ms = 0, z_max_runde = 0;

    uint64_t z_aend = 0, z_bytes_geaendert = 0, z_laeufe = 0, z_vollp = 0, z_leitung = 0;
    auto z_seit = Uhr::now(), runde_t = Uhr::now();
    auto break_um = Uhr::now() - std::chrono::hours(1);
    uint64_t bytes_bei_break = 0;
    bool break_offen = false;
    auto letztes_idle = Uhr::now();
    auto letzte_neubildung = Uhr::now() - std::chrono::seconds(10);
    uint32_t bilder = 0;

    uint32_t bilder_verlauf[4] = {};

    uint32_t letzte_bildnummer = 0;
    bool bildnummer_da = false;
    size_t bilder_pos = 0;
    uint64_t bytes_gesamt = 0;
    bool kabel_gemeldet = false, daten_gemeldet = false, aktuell_gemeldet = false, menue_gemeldet = false;
    auto letztes_menue = Uhr::now() - std::chrono::seconds(60);
    std::string letzter_fehler;
    std::vector<std::string> offene_hashes;
    auto letzte_aufloesung = Uhr::now();

    auto VerbindungMelden = [&](bool offen, bool daten, bool aktuell, bool menue) {
        if (offen == kabel_gemeldet && daten == daten_gemeldet && aktuell == aktuell_gemeldet && menue == menue_gemeldet) return;
        menue_gemeldet = menue;
        kabel_gemeldet = offen;
        daten_gemeldet = daten;
        aktuell_gemeldet = aktuell;
        SitzungsMeldung m;
        m.art = SitzungsMeldung::Art::Verbindung;
        m.kabel_offen = offen;
        m.daten_kommen = daten;
        m.daten_aktuell = aktuell;
        m.im_menue = menue;
        melder_(m);
    };

    while (!stopp_) {

        if (!port.Offen()) {
            if (Ms(letzter_versuch) >= 2000) {
                letzter_versuch = Uhr::now();

                std::vector<std::string> kandidaten;
                if (!port_name.empty()) kandidaten.push_back(port_name);
                for (const std::string& p : Cp210xPortsSuchen())
                    if (p != port_name) kandidaten.push_back(p);
                std::string fehler = kandidaten.empty() ? T("Kein Coder's Cable gefunden (CP210x nicht angeschlossen).")
                                                        : "";
                for (const std::string& p : kandidaten) {
                    if (port.Oeffnen(p, kBaud, fehler)) {
                        port_name = p;
                        k.Melden(SitzungsMeldung::Art::Gut, Fmt(T("%1 geöffnet."), {p}));
                        SitzungsMeldung m;
                        m.art = SitzungsMeldung::Art::Port;
                        m.text = p;
                        melder_(m);
                        k.liste_bestaetigt = false;
                        letzter_fehler.clear();
                        break;
                    }
                }
                if (!port.Offen() && fehler != letzter_fehler) {
                    letzter_fehler = fehler;
                    k.Melden(SitzungsMeldung::Art::Warnung, fehler);
                }
            }
        }

        if (port.Offen()) {
            const int n = port.Lesen(puffer.data(), puffer.size());
            if (n < 0) {
                port.Schliessen();
                k.Melden(SitzungsMeldung::Art::Warnung, Fmt(T("%1 getrennt."), {port_name}));
            } else if (n > 0) {
                leser.Einlesen(puffer.data(), static_cast<size_t>(n));
                bytes_gesamt += static_cast<uint64_t>(n);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        bool konsole_aus = false;
        if (Ms(letzte_cts_abfrage) >= 200) {
            letzte_cts_abfrage = Uhr::now();
            const int cts = port.Cts();
            if (cts >= 0 && cts != cts_zustand) {
                cts_zustand = cts;
                cts_seit = Uhr::now();
                if (cts == 1 && !k.erster_hash.empty()) cts_im_spiel = true;
            }
            if (cts == 1 && !k.erster_hash.empty()) cts_im_spiel = true;
            if (cts == 0 && cts_im_spiel && Ms(cts_seit) > 1500) {
                konsole_aus = true;
                cts_im_spiel = false;
            }

            if (port.BreakErkannt()) {
                break_um = Uhr::now();
                bytes_bei_break = bytes_gesamt;
                break_offen = true;
            }

            if (break_offen && Ms(break_um) > 2000) {
                break_offen = false;
                if (bytes_gesamt == bytes_bei_break && !k.erster_hash.empty() && Ms(letzte_kennung) > 15000)
                    konsole_aus = true;
            }
        }

        if (leser.MenueZeichen() || konsole_aus) {
            const bool neu_im_menue = konsole_aus || Ms(letztes_menue) > 3000;
            if (!konsole_aus) letztes_menue = Uhr::now();
            cts_im_spiel = false;
            if (neu_im_menue && (k.spiel_geladen || !k.erster_hash.empty())) {
                rc_client_unload_game(k.client);
                k.spiel_geladen = false;
                k.erster_hash.clear();
                k.hash.clear();

                k.geladener_hash.clear();
                offene_hashes.clear();
                k.adressen.clear();
                k.bereiche.clear();
                k.listen.clear();
                k.staende.clear();
                k.adressen_neu = false;
                letztes_paket = Uhr::now() - std::chrono::seconds(120);
                if (konsole_aus) letztes_paket = Uhr::now() - std::chrono::hours(1);
                k.Melden(SitzungsMeldung::Art::Log, konsole_aus ? "Konsole aus oder neu gestartet, Spiel beendet."
                                                                : "Konsole im DC-RAW-Menü, Spiel beendet.");
                if (konsole_aus) VerbindungMelden(port.Offen(), false, false, false);
            }
        }

        {
            Paketleser::KabelPaket kp;
            while (leser.NaechstesKabelpaket(kp)) {
                letztes_menue = Uhr::now();
                if (kp.typ == 0) {
                    kt_aktiv = true;
                    kt_start = Uhr::now();
                    kt_muster_ok = kt_muster_n = kt_rts_n = 0;
                    kt_cts[0] = kt_cts[1] = -1;
                    port.Flusssteuerung(false);
                    k.Melden(SitzungsMeldung::Art::Log, "Kabeltest gestartet ...");
                } else if (kp.typ == 2 && kt_aktiv) {
                    ++kt_muster_n;
                    bool gleich = kp.daten.size() == kKtMuster;
                    for (size_t i = 0; gleich && i < kKtMuster; ++i) gleich = kp.daten[i] == KtMuster(i);
                    if (gleich) ++kt_muster_ok;
                } else if (kp.typ == 3 && kt_aktiv && !kp.daten.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    kt_cts[kp.daten[0] ? 1 : 0] = port.Cts();
                    if (++kt_rts_n == 4) {

                        std::this_thread::sleep_for(std::chrono::milliseconds(400));
                        std::vector<uint8_t> t = {'K', 'T'};
                        for (size_t i = 0; i < kKtMuster; ++i) t.push_back(KtMuster(i));
                        for (size_t i = 0; i < t.size(); i += 8) {
                            port.Schreiben(t.data() + i, std::min<size_t>(8, t.size() - i));
                            std::this_thread::sleep_for(std::chrono::milliseconds(3));
                        }
                        for (uint8_t v = 0; v < 2; ++v) {
                            port.Rts(v != 0);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            const uint8_t r[3] = {'K', 'R', v};
                            port.Schreiben(r, 3);
                            std::this_thread::sleep_for(std::chrono::milliseconds(150));
                        }
                        port.Rts(true);
                    }
                } else if (kp.typ == 1 && kp.daten.size() >= 7) {
                    const unsigned empf = kp.daten[0] | (kp.daten[1] << 8), richtig = kp.daten[2] | (kp.daten[3] << 8);
                    const int c0 = static_cast<int8_t>(kp.daten[5]), c1 = static_cast<int8_t>(kp.daten[6]);
                    auto ok = [](bool b) { return std::string(T(b ? "in Ordnung" : "GESTÖRT")); };
                    auto z = [](int v) { return std::to_string(v); };
                    k.Melden(SitzungsMeldung::Art::Log,
                             Fmt(T("Kabeltest Dreamcast → PC (TX2 → RXD): %1 von %2 Testmustern richtig – %3"),
                                 {z(kt_muster_ok), z(kt_muster_n), ok(kt_muster_n > 0 && kt_muster_ok == kt_muster_n)}));
                    k.Melden(SitzungsMeldung::Art::Log,
                             Fmt(T("Kabeltest PC → Dreamcast (TXD → RX2): %1 Byte empfangen, Muster %2 von %3, Fehlerflags %4 – %5"),
                                 {z(empf), z(richtig), z(kKtMuster), z(kp.daten[4]), ok(richtig == kKtMuster)}));
                    k.Melden(SitzungsMeldung::Art::Log,
                             Fmt(T("Kabeltest RTS Dreamcast → CTS PC: bei 0 = %1, bei 1 = %2 – %3"),
                                 {z(kt_cts[0]), z(kt_cts[1]), ok(kt_cts[0] >= 0 && kt_cts[1] >= 0 && kt_cts[0] != kt_cts[1])}));
                    k.Melden(SitzungsMeldung::Art::Log,
                             Fmt(T("Kabeltest RTS PC → CTS Dreamcast: bei 0 = %1, bei 1 = %2 – %3"),
                                 {z(c0), z(c1), ok(c0 >= 0 && c1 >= 0 && c0 != c1)}));
                    kt_aktiv = false;
                    port.Flusssteuerung(true);
                }
            }
            if (kt_aktiv && Ms(kt_start) > 12000) {
                kt_aktiv = false;
                port.Flusssteuerung(true);
                k.Melden(SitzungsMeldung::Art::Warnung,
                         Fmt(T("Kabeltest: kein Ergebnis von der Dreamcast (Dreamcast → PC: %1 von %2 Testmustern richtig)."),
                             {std::to_string(kt_muster_ok), std::to_string(kt_muster_n)}));
            }
        }

        {
            uint32_t br[3];
            if (leser.NaechsteBildrate(br)) {
                if (br_da && br[0] > br_vorher[0]) {
                    const double vb = br[0] - br_vorher[0];
                    spiel_bilder_s = static_cast<float>((br[1] - br_vorher[1]) * 60.0 / vb);
                    rechenanteil = static_cast<float>((br[2] - br_vorher[2]) * 100.0 / (vb * 525.0));
                }
                for (int i = 0; i < 3; ++i) br_vorher[i] = br[i];
                br_da = true;
            }
        }
        if (leser.UeberschriebenMeldungen())
            k.Melden(SitzungsMeldung::Art::Warnung,
                     "Bei diesem Spiel können keine Achievements ausgewertet werden; das Spiel läuft normal weiter.");
        {
            core::Paketleser::GdPaket g;
            while (leser.NaechstesGdPaket(g)) {
            }
        }
        if (leser.NaechsteZaehler(diag_w)) {
            diag_da = true;
        }

        HashPaket hp;
        while (leser.NaechstesHashpaket(hp)) {
            letztes_paket = Uhr::now();
            letzte_kennung = Uhr::now();

            letztes_menue = Uhr::now() - std::chrono::hours(1);
            VerbindungMelden(port.Offen(), true, true, false);
            if (hp.hashes.empty() || hp.hashes[0] == k.erster_hash) continue;
            k.erster_hash = hp.hashes[0];
            {
                SitzungsMeldung m;
                m.art = SitzungsMeldung::Art::Kennung;
                m.titel = hp.titel;
                m.text = Fmt(T("Dreamcast meldet Spiel: %1"), {hp.titel});
                melder_(m);
            }
            k.titel = hp.titel;
            offene_hashes = hp.hashes;
            letzte_aufloesung = Uhr::now() - std::chrono::minutes(5);
        }

        if (!offene_hashes.empty() && Ms(letzte_aufloesung) >= 60000) {
            letzte_aufloesung = Uhr::now();
            k.hash.clear();
            std::string fehler;
            std::set<std::string> geprueft;
            bool netz_vorher = false;
            for (size_t i = 0; i < offene_hashes.size() && k.hash.empty() && fehler.empty(); ++i) {
                if (!geprueft.insert(offene_hashes[i]).second) continue;

                uint32_t im_cache = 0;
                const bool netz = !HashCache::Global().Nachsehen(offene_hashes[i], im_cache) || im_cache == 0;
                if (netz && netz_vorher) std::this_thread::sleep_for(std::chrono::milliseconds(1100));
                netz_vorher = netz_vorher || netz;
                const HashAufloesung a = HashCache::Global().Aufloesen(offene_hashes[i], true);
                if (!a.ok) { fehler = a.fehler; break; }
                if (a.spiel_id) {
                    k.hash = offene_hashes[i];
                }
            }
            if (!fehler.empty()) {
                k.Melden(SitzungsMeldung::Art::Warnung,
                         Fmt(T("RA-Abfrage fehlgeschlagen (%1), neuer Versuch in 1 Minute."), {T(fehler)}));
            } else {
                offene_hashes.clear();
                if (k.hash.empty()) {

                    SitzungsMeldung m;
                    m.art = SitzungsMeldung::Art::Kennung;
                    m.titel = k.titel;
                    m.unbekannt = true;
                    m.text = Fmt(T("%1: dieses Abbild kennt RetroAchievements nicht."), {k.titel});
                    melder_(m);
                } else
                    SpielLadenWennBereit(k);
            }
        }

        DatenPaket dp;
        while (leser.NaechstesDatenpaket(dp)) {
            letztes_paket = Uhr::now();
            ++z_an;

            auto li = k.listen.find(dp.id);
            if (li == k.listen.end() && !(dp.id == kVorgabeListe && !k.vorgabe.empty())) { ++z_fremd; continue; }

            const std::vector<Bereich>& liste = li != k.listen.end() ? li->second : k.vorgabe;
            size_t soll = 0;
            for (const Bereich& b : liste) soll += b.laenge;

            std::vector<uint8_t>& stand = k.staende[dp.id];
            if (!dp.aenderung) {
                if (dp.daten.size() != soll) { ++z_voll; continue; }
                ++z_vollp;
                z_leitung += 13 + dp.daten.size();
                stand = dp.daten;
            } else {
                if (stand.size() != soll) { ++z_ohne; continue; }
                ++z_aend;
                z_leitung += 13 + dp.daten.size();
                size_t laeufe = 0, bytes = 0;
                if (!AenderungAnwenden(dp, stand, &laeufe, &bytes)) { ++z_ohne; continue; }
                z_laeufe += laeufe;
                z_bytes_geaendert += bytes;
            }
            if (dp.id == k.liste_id) k.liste_bestaetigt = true;
            size_t pos = 0;
            for (const Bereich& b : liste) {
                std::memcpy(&k.spiegel[b.adresse], &stand[pos], b.laenge);
                for (uint32_t i = 0; i < b.laenge; ++i) k.empfangen[b.adresse + i] = true;
                pos += b.laenge;
            }
            if (k.spiel_geladen) {
                k.fehlende_gelesen = false;
                uint32_t schritte = 1;
                if (bildnummer_da && dp.bild > letzte_bildnummer && dp.bild - letzte_bildnummer <= 4)
                    schritte = dp.bild - letzte_bildnummer;
                letzte_bildnummer = dp.bild;
                bildnummer_da = true;
                for (uint32_t i = 0; i < schritte; ++i) rc_client_do_frame(k.client);
                letztes_idle = Uhr::now();
                bilder += schritte;
                ++z_aus;

                static Uhr::time_point letzter_hinweis{};
            }
        }

        const bool still = Ms(letztes_paket) > 3000;
        if (still && !k.bereiche.empty()) k.liste_bestaetigt = false;

        if (port.Offen() && (k.erster_hash.empty() || still) && Ms(letzte_hashanfrage) >= 1000 &&
            Ms(letztes_menue) > 3000) {
            letzte_hashanfrage = Uhr::now();
            const auto a = AnfrageHash();
            port.Schreiben(a.data(), a.size());
        }

        if (port.Offen() && k.adressen_neu && Ms(letzte_neubildung) >= 2000) {
            k.adressen_neu = false;
            letzte_neubildung = Uhr::now();
            std::vector<Bereich> neu;
            if (!BereicheBilden(k.adressen, neu) && !k.grenze_gemeldet) {
                k.grenze_gemeldet = true;
                size_t bytes = 0;
                for (const Bereich& b : neu) bytes += b.laenge;
                (void)bytes;
                k.Melden(SitzungsMeldung::Art::Warnung,
                         T("Das Achievement-Set dieses Spiels ist sehr groß; einzelne Achievements werden eventuell nicht ausgewertet."));
            }
            k.bereiche = neu;
            k.liste_id = static_cast<uint8_t>(k.liste_id + 1);

            while (k.liste_id == kVorgabeListe || k.liste_id == kAbzugListe) k.liste_id = static_cast<uint8_t>(k.liste_id + 1);
            k.listen[k.liste_id] = neu;
            k.staende.erase(k.liste_id);
            while (k.listen.size() > 8) {
                uint8_t aelteste = k.listen.begin()->first;
                uint8_t abstand = 0;
                for (const auto& e : k.listen)
                    if (static_cast<uint8_t>(k.liste_id - e.first) > abstand) {
                        abstand = static_cast<uint8_t>(k.liste_id - e.first);
                        aelteste = e.first;
                    }
                k.listen.erase(aelteste);
                k.staende.erase(aelteste);
            }
            k.liste_bestaetigt = false;
            letzte_liste = Uhr::now() - std::chrono::seconds(10);
        }

        if (versuche_fuer != k.liste_id) { versuche_fuer = k.liste_id; listen_versuche = 0; }
        if (port.Offen() && !abzug.aktiv && !k.liste_bestaetigt && !k.bereiche.empty() && Ms(letzte_liste) >= 1500 &&
            listen_versuche < 4) {
            ++listen_versuche;
            letzte_liste = Uhr::now();

            const auto a = AnfrageBereiche(k.liste_id, k.bereiche);
            size_t geschrieben = 0;
            const auto t0 = Uhr::now();
            for (size_t i = 0; i < a.size(); i += 16) {
                const size_t n = std::min<size_t>(16, a.size() - i);
                if (!port.Schreiben(a.data() + i, n)) break;
                geschrieben += n;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            z_liste_ms += Ms(t0);
        }

        z_max_runde = std::max<int64_t>(z_max_runde, Ms(runde_t));
        runde_t = Uhr::now();

        HttpErgebnis e;
        while (k.http.Fertig(e)) {
            rc_api_server_response_t r = {};
            r.body = e.antwort.koerper.c_str();
            r.body_length = e.antwort.koerper.size();
            r.http_status_code = e.antwort.status ? e.antwort.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
            e.auftrag.rueckruf(&r, e.auftrag.rueckruf_daten);
        }

        if (Ms(letztes_idle) >= 1000) {
            letztes_idle = Uhr::now();
            rc_client_idle(k.client);
        }

        if (Ms(letzte_statistik) >= 1000) {

            const double dauer_s = std::max(0.5, Ms(letzte_statistik) / 1000.0);
            letzte_statistik = Uhr::now();

            VerbindungMelden(port.Offen(),
                             Ms(letztes_paket) < (k.erster_hash.empty() ? 60000 : 600000) && Ms(letztes_menue) >= 3000,
                             Ms(letztes_paket) < 3000, Ms(letztes_menue) < 3000);
            SitzungsMeldung m;
            m.art = SitzungsMeldung::Art::Statistik;
            {
                bilder_verlauf[bilder_pos++ % 4] = static_cast<uint32_t>(bilder / dauer_s + 0.5);
                uint32_t summe = 0, n = 0;
                for (uint32_t v : bilder_verlauf) { summe += v; ++n; }
                const size_t gefuellt = std::min<size_t>(bilder_pos, 4);
                m.bilder_pro_s = bilder == 0 ? 0 : (summe + static_cast<uint32_t>(gefuellt) / 2) / static_cast<uint32_t>(gefuellt);
                (void)n;
            }
            m.spiel_bilder_s = spiel_bilder_s;
            m.rechenanteil = rechenanteil;
            m.bereiche = static_cast<uint32_t>(k.bereiche.size());
            size_t soll = 0;
            for (const Bereich& b : k.bereiche) soll += b.laenge;
            m.bytes_pro_bild = static_cast<uint32_t>(soll);
            m.pruefsummenfehler = leser.Fehler();
            m.bytes_empfangen = bytes_gesamt;
            m.adressen_gesamt = static_cast<uint32_t>(k.adressen.size());

            if (diag_da) {
                m.diag_w0 = diag_w[0]; m.diag_w1 = diag_w[1]; m.diag_w2 = diag_w[2];
                m.diag_w3 = diag_w[3]; m.diag_w4 = diag_w[4];
                m.diag_w5 = diag_w[5]; m.diag_w6 = diag_w[6]; m.diag_w7 = diag_w[7];
                m.diag_w8 = diag_w[8]; m.diag_w9 = diag_w[9];
                m.diag_ok = true;
            }
            melder_(m);
            bilder = 0;
        }

        if (Ms(letzte_ramanzeige) >= 250) {
            letzte_ramanzeige = Uhr::now();
            SitzungsMeldung m;
            m.art = SitzungsMeldung::Art::Ram;
            size_t i = 0;
            for (uint32_t a : k.adressen) {
                if (i >= 64) break;
                if (k.vorher.size() <= i) k.vorher.push_back(k.spiegel[a]);
                RamZeile z;
                z.adresse = a;
                z.wert = k.spiegel[a];
                z.vorher = k.vorher[i];
                z.empfangen = k.empfangen[a];
                m.ram.push_back(z);
                k.vorher[i] = k.spiegel[a];
                ++i;
            }
            m.adressen_gesamt = static_cast<uint32_t>(k.adressen.size());
            melder_(m);
        }
    }

    port.Schliessen();
    rc_client_destroy(k.client);
    VerbindungMelden(false, false, false, false);
}

}
