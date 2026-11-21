// DREAM-RAW: window, pages and dialogs.

#include <windows.h>
#include <winioctl.h>
#include <gl/gl.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl2.h"
#include "imgui_impl_win32.h"

#include "buildinfo.h"
#include "i18n.h"
#include "core/adressliste.h"
#include "core/einstellungen.h"
#include "core/gdiabbild.h"
#include "core/http.h"
#include "core/ra.h"
#include "core/sitzung.h"
#include "core/text.h"
#include "bilder.h"
#include "erzeugt_katze.h"
#include "icons.h"
#include "popup.h"
#include "theme.h"

extern const uint8_t kDreamRawProgramm[];
extern const size_t kDreamRawGroesse;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using theme::Zustand;

HDC   g_dc = nullptr;
HGLRC g_rc = nullptr;
HWND  g_fenster = nullptr;
float g_dpi = 1.0f;

float D(float v) { return v * g_dpi; }

std::mutex g_auftrag_sperre;
std::vector<std::function<void()>> g_auftraege;

void ImHauptthread(std::function<void()> f) {
    std::lock_guard<std::mutex> l(g_auftrag_sperre);
    g_auftraege.push_back(std::move(f));
}

void AuftraegeAusfuehren() {
    std::vector<std::function<void()>> liste;
    {
        std::lock_guard<std::mutex> l(g_auftrag_sperre);
        liste.swap(g_auftraege);
    }
    for (auto& f : liste) f();
}

enum class Seite { Spiel, Konto, Netz, Log, Setup, Info };

struct Logzeile {
    std::string zeit;
    std::string text;
    Zustand art;
};

struct Zustand_ {
    Seite seite = Seite::Spiel;
    std::vector<Logzeile> log;

    char ra_benutzer[128] = {};
    char ra_passwort[256] = {};
    char dreampi_host[128] = {};
    char dc_host[128] = {};
    char dc_port[16] = {};

    bool angemeldet_bleiben = false;
    long long nachricht_gelesen = 0;
    std::string sprache;
    std::string gespeichertes_token;
    bool angemeldet = false;
    std::string benutzer;
    std::string token;
    uint32_t punkte = 0;

    uint32_t spiel_id = 0;
    core::SpielDaten spiel;

    char kabel_port[16] = "";
    bool kabel_offen = false;
    bool daten_kommen = false;
    bool daten_aktuell = false;
    bool im_menue = false;
    bool dc_unbekannt = false;
    std::string verbindung_weg;
    std::string dc_titel;
    uint32_t bilder_pro_s = 0;
    float spiel_bilder_s = -1, rechenanteil = -1;
    uint32_t bereiche = 0;
    uint32_t bytes_pro_bild = 0;
    uint64_t pruefsummenfehler = 0;
    core::SitzungsMeldung diag;
    uint64_t bytes_empfangen = 0;
    std::vector<uint32_t> freigeschaltet;
    std::vector<core::RamZeile> ram;
    uint32_t adressen_gesamt = 0;
    bool zeige_ram = true;
    std::map<uint32_t, double> geaendert_um;
    bool animation_gespielt = false;

    int laeuft = 0;
};
Zustand_ g_z;
std::wstring g_einstellungs_pfad;

double g_seq_start = -1.0;
extern float g_deckel;
void SeqStarten();
bool g_seq_vorschau = false;

FILE* LogDatei() {
    static FILE* f = []() -> FILE* {
        wchar_t p[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, p, MAX_PATH);
        std::wstring pfad(p);
        const size_t s = pfad.find_last_of(L"\\/");
        pfad = (s == std::wstring::npos ? L"" : pfad.substr(0, s + 1)) + L"dream_raw_log.txt";
        return _wfopen(pfad.c_str(), L"w");
    }();
    return f;
}

void Log(const std::string& text_de, Zustand art = Zustand::Neutral) {
    const std::string text = T(text_de);
    char zeit[16];
    const std::time_t t = std::time(nullptr);
    std::strftime(zeit, sizeof(zeit), "%H:%M:%S", std::localtime(&t));
    g_z.log.push_back({zeit, text, art});
    if (FILE* f = LogDatei()) {
        std::fprintf(f, "%s  %s\n", zeit, text.c_str());
        std::fflush(f);
    }
}

void Nebenher(std::function<std::function<void()>()> arbeit) {
    ++g_z.laeuft;
    std::thread([arbeit]() {
        std::function<void()> fertig = arbeit();
        ImHauptthread([fertig]() {
            --g_z.laeuft;
            if (fertig) fertig();
        });
    }).detach();
}

void Kopieren(char* ziel, size_t groesse, const std::string& quelle) {
    std::snprintf(ziel, groesse, "%s", quelle.c_str());
}

void EinstellungenLaden() {
    core::Einstellungen e;
    if (g_einstellungs_pfad.empty() || !core::EinstellungenLaden(g_einstellungs_pfad, e)) return;
    Kopieren(g_z.ra_benutzer, sizeof(g_z.ra_benutzer), e.ra_benutzer);
    Kopieren(g_z.dreampi_host, sizeof(g_z.dreampi_host), e.dreampi_host);
    Kopieren(g_z.dc_host, sizeof(g_z.dc_host), e.dreamshell_host);
    g_z.dc_port[0] = '\0';
    if (e.dreamshell_port > 0) std::snprintf(g_z.dc_port, sizeof(g_z.dc_port), "%d", e.dreamshell_port);
    if (!e.kabel_port.empty()) Kopieren(g_z.kabel_port, sizeof(g_z.kabel_port), e.kabel_port);
    g_z.angemeldet_bleiben = e.angemeldet_bleiben;
    g_z.gespeichertes_token = e.angemeldet_bleiben ? e.ra_token : "";
    g_z.nachricht_gelesen = e.nachricht_gelesen;
    g_z.sprache = e.sprache;
    i18n::SpracheSetzen(e.sprache.empty() ? i18n::SystemSprache() : e.sprache);
    Log("Einstellungen geladen.");
}

void EinstellungenSpeichern() {
    core::Einstellungen e;
    e.ra_benutzer = core::Getrimmt(g_z.ra_benutzer);
    e.dreampi_host = core::Getrimmt(g_z.dreampi_host);
    e.dreamshell_host = core::Getrimmt(g_z.dc_host);
    e.kabel_port = core::Getrimmt(g_z.kabel_port);
    e.angemeldet_bleiben = g_z.angemeldet_bleiben;
    e.nachricht_gelesen = g_z.nachricht_gelesen;
    e.sprache = g_z.sprache;
    e.ra_token = g_z.angemeldet_bleiben ? (g_z.angemeldet ? g_z.token : g_z.gespeichertes_token) : "";
    if (core::Getrimmt(g_z.dc_port).empty()) {
        e.dreamshell_port = 0;
    } else {
        e.dreamshell_port = core::PortLesen(g_z.dc_port);
        if (e.dreamshell_port < 0) {
            Log("Nicht gespeichert: Port muss zwischen 1 und 65535 liegen.", Zustand::Fehler);
            return;
        }
    }
    if (g_einstellungs_pfad.empty() || !core::EinstellungenSpeichern(g_einstellungs_pfad, e)) {
        Log("Einstellungen konnten nicht gespeichert werden.", Zustand::Fehler);
        return;
    }
    Log("Einstellungen gespeichert (Passwort wird nicht gespeichert).", Zustand::Gut);
}

void SpielLaden(uint32_t id);
void KabelVerbinden();

void Anmelden() {
    const std::string u = core::Getrimmt(g_z.ra_benutzer), p = g_z.ra_passwort;
    if (u.empty() || p.empty()) { Log("Bitte Benutzername und Passwort eingeben.", Zustand::Warnung); return; }
    Log("Anmeldung läuft ...");
    Nebenher([u, p]() -> std::function<void()> {
        const core::Anmeldung a = core::Anmelden(u, p);
        return [a]() {
            if (!a.ok) { Log(Fmt(T("Anmeldung fehlgeschlagen: %1"), {T(a.fehler)}), Zustand::Fehler); return; }
            g_z.angemeldet = true;
            g_z.benutzer = a.benutzer;
            g_z.token = a.token;
            g_z.punkte = a.punkte_softcore;
            Log(Fmt(T("Angemeldet als %1 (%2 Punkte)."), {a.benutzer, std::to_string(a.punkte_softcore)}), Zustand::Gut);
            if (g_z.angemeldet_bleiben) EinstellungenSpeichern();
            if (g_z.spiel_id && g_z.spiel.id != g_z.spiel_id) SpielLaden(g_z.spiel_id);
            KabelVerbinden();
        };
    });

    SecureZeroMemory(g_z.ra_passwort, sizeof(g_z.ra_passwort));
}

void AutomatischAnmelden() {
    const std::string u = core::Getrimmt(g_z.ra_benutzer), t = g_z.gespeichertes_token;
    if (!g_z.angemeldet_bleiben || u.empty() || t.empty()) return;
    Log("Automatische Anmeldung mit gespeichertem Sitzungstoken ...");
    Nebenher([u, t]() -> std::function<void()> {
        const core::Anmeldung a = core::AnmeldenMitToken(u, t);
        return [a]() {
            if (!a.ok) {
                Log(Fmt(T("Automatische Anmeldung fehlgeschlagen (%1). Bitte das Passwort eingeben."), {T(a.fehler)}),
                    Zustand::Warnung);
                return;
            }
            g_z.angemeldet = true;
            g_z.benutzer = a.benutzer;
            g_z.token = a.token.empty() ? g_z.gespeichertes_token : a.token;
            g_z.punkte = a.punkte_softcore;
            Log(Fmt(T("Automatisch angemeldet als %1 (%2 Punkte)."), {a.benutzer, std::to_string(a.punkte_softcore)}),
                Zustand::Gut);
            KabelVerbinden();
        };
    });
}

void Abmelden() {
    g_z.angemeldet = false;
    g_z.benutzer.clear();
    SecureZeroMemory(&g_z.token[0], g_z.token.size());
    g_z.token.clear();
    g_z.gespeichertes_token.clear();
    g_z.punkte = 0;
    if (g_z.angemeldet_bleiben) EinstellungenSpeichern();
    Log("Abgemeldet.");
}

void SpielLaden(uint32_t id) {
    if (!g_z.angemeldet) { Log("Die Achievement-Liste braucht eine Anmeldung.", Zustand::Warnung); return; }
    const std::string u = g_z.benutzer, t = g_z.token;
    Nebenher([u, t, id]() -> std::function<void()> {
        const core::SpielDaten d = core::SpielLaden(u, t, id);
        return [d]() {
            if (!d.ok) { Log(Fmt(T("Spieldaten fehlgeschlagen: %1"), {T(d.fehler)}), Zustand::Fehler); return; }
            g_z.spiel = d;
            Log(Fmt(T("Spiel %1: %2, %3 Achievements."),
                    {std::to_string(d.id), d.titel, std::to_string(d.achievements.size())}),
                Zustand::Gut);
            if (d.konsole != core::kKonsoleDreamcast)
                Log("Das Spiel gehört laut Server nicht zur Konsole Dreamcast (40).", Zustand::Warnung);
        };
    });
}

void FreischaltTon() {
    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring wav = exe;
    wav = wav.substr(0, wav.find_last_of(L"\\/") + 1) + L"achievement.wav";
    if (::GetFileAttributesW(wav.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ::PlaySoundW(wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC);
        return;
    }
    std::thread([]() {
        static const int melodie[][2] = {{587, 140}, {659, 140}, {698, 140}, {784, 140}, {880, 320},
                                         {1175, 200}, {880, 160}, {698, 140}, {784, 140}, {880, 420}};
        for (auto& n : melodie) ::Beep(static_cast<DWORD>(n[0]), static_cast<DWORD>(n[1]));
    }).detach();
}

std::string BadgePng(const std::string& name) {
    if (name.empty() || name.find_first_of("/\\.:") != std::string::npos) return {};
    const wchar_t* a = _wgetenv(L"APPDATA");
    std::wstring datei;
    if (a) {
        const std::wstring ordner = std::wstring(a) + L"\\DREAM-RAW\\badge_cache";
        ::CreateDirectoryW((std::wstring(a) + L"\\DREAM-RAW").c_str(), nullptr);
        ::CreateDirectoryW(ordner.c_str(), nullptr);
        datei = ordner + L"\\" + core::NachWide(name) + L".png";
        if (FILE* f = _wfopen(datei.c_str(), L"rb")) {
            std::string png;
            char puffer[4096];
            size_t n;
            while ((n = std::fread(puffer, 1, sizeof(puffer), f)) > 0) png.append(puffer, n);
            std::fclose(f);
            if (!png.empty()) return png;
        }
    }
    const core::HttpAntwort h = core::HttpAnfrage("https://media.retroachievements.org/Badge/" + name + ".png", "", 8000);
    if (!h.ok || h.status != 200 || h.koerper.size() < 8 || h.koerper.compare(1, 3, "PNG") != 0) return {};
    if (!datei.empty())
        if (FILE* f = _wfopen(datei.c_str(), L"wb")) {
            std::fwrite(h.koerper.data(), 1, h.koerper.size(), f);
            std::fclose(f);
        }
    return h.koerper;
}

void FreischaltungZeigen(const core::SitzungsMeldung& m) {
    const std::string titel = m.titel, beschreibung = m.beschreibung, badge = m.badge;
    const int punkte = static_cast<int>(m.punkte);
    Nebenher([titel, beschreibung, badge, punkte]() -> std::function<void()> {
        const std::string png = BadgePng(badge);
        return [titel, beschreibung, punkte, png]() {
            HDC dc = ::GetDC(nullptr);
            const float skalierung = dc ? ::GetDeviceCaps(dc, LOGPIXELSX) / 96.0f : 1.0f;
            if (dc) ::ReleaseDC(nullptr, dc);
            popup::Zeigen(titel, beschreibung, punkte, png, T("ACHIEVEMENT FREIGESCHALTET!"), T("Pkt"), skalierung,
                          []() { FreischaltTon(); });
        };
    });
}

void SitzungsMeldungVerarbeiten(const core::SitzungsMeldung& m) {
    using A = core::SitzungsMeldung::Art;
    switch (m.art) {
        case A::Log: Log(m.text); break;
        case A::Warnung: Log(m.text, Zustand::Warnung); break;
        case A::Fehler: Log(m.text, Zustand::Fehler); break;
        case A::Gut: Log(m.text, Zustand::Gut); break;
        case A::Verbindung:

            if (g_z.kabel_offen && !m.kabel_offen) {
                g_z.verbindung_weg = T("Kabel getrennt. Das Tool sucht es weiter; bitte USB-Stecker prüfen.");
            } else if (g_z.daten_kommen && !m.daten_kommen && !m.im_menue) {
                g_z.verbindung_weg = T("Seit längerer Zeit keine Daten von der Dreamcast: Konsole aus oder Kabel lose. "
                                       "Achievements werden in dieser Zeit nicht ausgewertet.");
            }
            if (!g_z.verbindung_weg.empty() && (g_z.kabel_offen != m.kabel_offen || g_z.daten_kommen != m.daten_kommen) &&
                !m.daten_kommen) {
                Log(Fmt(T("Verbindung verloren: %1"), {g_z.verbindung_weg}), Zustand::Fehler);
                FLASHWINFO fl = {sizeof(fl), g_fenster, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
                ::FlashWindowEx(&fl);
            }
            if (m.daten_kommen && !g_z.verbindung_weg.empty()) {
                g_z.verbindung_weg.clear();
                Log("Verbindung wieder da, die Dreamcast sendet.", Zustand::Gut);
            }

            if (m.im_menue && !g_z.im_menue) {
                g_z.verbindung_weg.clear();
                g_z.spiel_id = 0;
                g_z.spiel = core::SpielDaten{};
                g_z.freigeschaltet.clear();
                g_z.animation_gespielt = false;
                g_z.dc_titel.clear();
                g_seq_start = -1.0;
                Log("Dreamcast ist im DC-RAW-Menü.", Zustand::Gut);
            }
            g_z.im_menue = m.im_menue;
            g_z.kabel_offen = m.kabel_offen;
            if (m.daten_kommen && !g_z.daten_kommen) {

                g_z.seite = Seite::Spiel;
                g_z.zeige_ram = true;

                if (!g_z.animation_gespielt && g_z.seite == Seite::Spiel && !g_z.dc_titel.empty()) {
                    g_z.animation_gespielt = true;
                    g_seq_vorschau = false;
                    SeqStarten();
                }
            }

            if (g_z.daten_kommen && !m.daten_kommen && !m.im_menue) {
                g_z.spiel_id = 0;
                g_z.spiel = core::SpielDaten{};
                g_z.freigeschaltet.clear();
                g_z.animation_gespielt = false;
                g_z.dc_titel.clear();
                g_z.ram.clear();
            }
            g_z.daten_kommen = m.daten_kommen;
            g_z.daten_aktuell = m.daten_aktuell;
            break;
        case A::Port:
            if (core::Getrimmt(g_z.kabel_port) != m.text) {
                Kopieren(g_z.kabel_port, sizeof(g_z.kabel_port), m.text);
                EinstellungenSpeichern();
            }
            break;
        case A::Spiel:
            Log(m.text, m.spiel_id ? Zustand::Gut : Zustand::Warnung);
            if (m.spiel_id && m.spiel_id != g_z.spiel_id) {
                if (g_z.spiel_id != 0) g_z.animation_gespielt = false;
                g_z.spiel_id = m.spiel_id;
                SpielLaden(m.spiel_id);
            }
            if (m.spiel_id) g_z.freigeschaltet = m.bereits;
            break;
        case A::Kennung:
            Log(m.text, m.unbekannt ? Zustand::Warnung : Zustand::Neutral);
            g_z.dc_titel = m.titel;
            g_z.dc_unbekannt = m.unbekannt;
            break;
        case A::Freigeschaltet:
            Log(m.text, Zustand::Gut);
            g_z.freigeschaltet.push_back(m.achievement_id);
            FreischaltungZeigen(m);
            break;
        case A::Statistik:
            g_z.bilder_pro_s = m.bilder_pro_s;
        g_z.spiel_bilder_s = m.spiel_bilder_s;
        g_z.rechenanteil = m.rechenanteil;
            g_z.bereiche = m.bereiche;
            g_z.bytes_pro_bild = m.bytes_pro_bild;
            g_z.pruefsummenfehler = m.pruefsummenfehler;
            if (m.diag_ok) g_z.diag = m;
            g_z.bytes_empfangen = m.bytes_empfangen;
            g_z.adressen_gesamt = m.adressen_gesamt;
            break;
        case A::Ram: {

            const double jetzt = ImGui::GetTime();
            for (const core::RamZeile& z : m.ram)
                if (z.empfangen && z.wert != z.vorher) g_z.geaendert_um[z.adresse] = jetzt;
            g_z.ram = m.ram;
            g_z.adressen_gesamt = m.adressen_gesamt;
            break;
        }
    }
}

core::Sitzung g_sitzung([](const core::SitzungsMeldung& m) {
    ImHauptthread([m]() { SitzungsMeldungVerarbeiten(m); });
});

void KabelVerbinden() {
    if (!g_z.angemeldet) { Log("Zuerst unter B · Konto anmelden.", Zustand::Warnung); return; }
    if (g_sitzung.Laeuft()) return;
    const std::string port = core::Getrimmt(g_z.kabel_port);
    Log(port.empty() ? std::string(T("Sitzung gestartet (Softcore), suche das Kabel ..."))
                     : Fmt(T("Sitzung gestartet (Softcore), suche das Kabel (zuerst %1) ..."), {port}));
    g_sitzung.Starten(port, g_z.benutzer, g_z.token);
}

void KabelTrennen() {
    g_sitzung.Beenden();
    g_z.kabel_offen = false;
    g_z.daten_kommen = false;
    g_z.daten_aktuell = false;
    g_z.im_menue = false;
    g_z.animation_gespielt = false;
    Log("Sitzung beendet.");
}

bool IstFreigeschaltet(uint32_t id) {
    for (uint32_t f : g_z.freigeschaltet) if (f == id) return true;
    return false;
}

ImVec4 F(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void Leise(const char* fmt, const char* wert) {
    fmt = T(fmt);
    wert = T(wert);
    ImGui::PushStyleColor(ImGuiCol_Text, F(theme::kLeise));
    ImGui::Text(fmt, wert);
    ImGui::PopStyleColor();
}

void Zeile(const char* beschriftung, const char* wert, bool mono = false) {
    beschriftung = T(beschriftung);
    wert = T(wert);
    ImGui::PushStyleColor(ImGuiCol_Text, F(theme::kLeise));
    ImGui::TextUnformatted(beschriftung);
    ImGui::PopStyleColor();
    ImGui::SameLine(D(110));
    if (mono) ImGui::PushFont(theme::gMono, theme::kGroesseKlein + 0.5f);
    ImGui::TextUnformatted(wert);
    if (mono) ImGui::PopFont();
}

void Abstand(float v) { ImGui::Dummy(ImVec2(0, D(v))); }

void RamTabelle() {
    if (g_z.ram.empty()) {
        theme::Hinweis("Noch keine Speicherstellen. Sie erscheinen, sobald ein Spiel mit Achievements läuft.");
        return;
    }
    const std::string kopf = Fmt(T(g_z.adressen_gesamt > g_z.ram.size() ? "%1 Adressen geprüft, die ersten 64 angezeigt"
                                                                       : "%1 Adressen geprüft"),
                                 {std::to_string(g_z.adressen_gesamt)});
    Leise("%s", kopf.c_str());
    Abstand(2);
    const ImGuiTableFlags f = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH;
    if (ImGui::BeginTable("ram", 3, f, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn(T("Adresse"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Wert"), ImGuiTableColumnFlags_WidthFixed, D(50));
        ImGui::TableSetupColumn(T("vorher"), ImGuiTableColumnFlags_WidthFixed, D(50));
        ImGui::TableHeadersRow();
        ImGui::PushFont(theme::gMono, theme::kGroesseKlein + 0.5f);
        for (const core::RamZeile& z : g_z.ram) {
            const bool geaendert = z.wert != z.vorher;
            const ImU32 farbe = !z.empfangen ? theme::kSehrLeise : geaendert ? theme::kAkzent : theme::kText;
            char t[16];
            ImGui::TableNextRow();
            ImGui::PushStyleColor(ImGuiCol_Text, F(farbe));
            ImGui::TableNextColumn();
            std::snprintf(t, sizeof(t), "0x%06X", z.adresse);
            ImGui::TextUnformatted(t);
            ImGui::TableNextColumn();
            std::snprintf(t, sizeof(t), "%02X", z.wert);
            ImGui::TextUnformatted(t);
            ImGui::TableNextColumn();
            std::snprintf(t, sizeof(t), "%02X", z.vorher);
            ImGui::TextUnformatted(t);
            ImGui::PopStyleColor();
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
}

void VerbindungsHinweis() {
    if (g_z.verbindung_weg.empty() || !g_sitzung.Laeuft()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, F(theme::kFehler));
    ImGui::PushFont(theme::gHalbfett, 0.0f);
    ImGui::TextWrapped("%s", T("Verbindung verloren"));
    ImGui::PopFont();
    ImGui::TextWrapped("%s", g_z.verbindung_weg.c_str());
    ImGui::PopStyleColor();
    Abstand(2);
}

void SpielInhalt(float unten_frei);
extern float g_roentgen;
void SeiteSpiel() {
    VerbindungsHinweis();
    if (g_sitzung.Laeuft() && g_roentgen > 0.5f) return;
    theme::Seitenkopf("Spiel", nullptr);
    SpielInhalt(0.0f);
}

void SpielInhalt(float unten_frei) {
    if (!g_z.spiel.ok) {
        if (g_z.spiel_id) {
            theme::Marke("erkannt", Zustand::Info);
            Abstand(2);
            char t[32];
            std::snprintf(t, sizeof(t), "%u", g_z.spiel_id);
            Zeile("Spiel-ID", t, true);
            if (!g_z.angemeldet) {
                Abstand(2);
                theme::Hinweis("Für die Achievement-Liste unter B · Konto anmelden.");
            }
        } else {
            theme::Marke("wartet auf die Dreamcast", Zustand::Neutral);
            Abstand(4);
            theme::Hinweis("Das Spiel wird automatisch erkannt, sobald die Dreamcast verbunden ist und ein "
                           "Spiel startet. Eine manuelle Auswahl ist nicht nötig.");
        }
        return;
    }

    ImGui::PushFont(theme::gHalbfett, theme::kGroesseZwisch);
    ImGui::TextWrapped("%s", g_z.spiel.titel.c_str());
    ImGui::PopFont();
    uint32_t summe = 0;
    for (const auto& a : g_z.spiel.achievements) summe += a.punkte;
    const std::string info = Fmt(T("%1 Achievements  ·  %2 Punkte"),
                                 {std::to_string(g_z.spiel.achievements.size()), std::to_string(summe)});
    Leise("%s", info.c_str());
    Abstand(2);

    const ImGuiTableFlags f = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH;
    if (ImGui::BeginTable("achievements", 2, f, ImVec2(0, std::max(D(40), ImGui::GetContentRegionAvail().y - unten_frei)))) {
        ImGui::TableSetupColumn(T("Punkte"), ImGuiTableColumnFlags_WidthFixed, D(44));
        ImGui::TableSetupColumn(T("Achievement"), ImGuiTableColumnFlags_WidthStretch);
        for (const auto& a : g_z.spiel.achievements) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(theme::gHalbfett, theme::kGroesseZwisch);
            ImGui::PushStyleColor(ImGuiCol_Text, F(theme::kAkzent));
            ImGui::Text("%u", a.punkte);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::PushFont(theme::gHalbfett, 0.0f);
            ImGui::TextWrapped("%s", a.titel.c_str());
            ImGui::PopFont();
            if (IstFreigeschaltet(a.id)) theme::Marke("freigeschaltet", Zustand::Gut);
            if (!a.offiziell) theme::Marke("inoffiziell", Zustand::Warnung);
            ImGui::PushStyleColor(ImGuiCol_Text, F(theme::kLeise));
            ImGui::TextWrapped("%s", a.beschreibung.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
}

void SeiteKonto() {
    theme::Seitenkopf("Konto", "Mit dem RetroAchievements-Konto anmelden.");
    if (g_z.angemeldet) {
        theme::Marke("angemeldet", Zustand::Gut);
        Abstand(2);
        Zeile("Benutzer", g_z.benutzer.c_str());
        char p[32];
        std::snprintf(p, sizeof(p), "%u", g_z.punkte);
        Zeile("Punkte", p);
        Abstand(6);
        if (theme::KnopfNeben("Abmelden")) Abmelden();
        return;
    }
    theme::Feld("Benutzername", "##ra_user", g_z.ra_benutzer, sizeof(g_z.ra_benutzer));
    const bool enter = theme::Feld("Passwort", "##ra_pass", g_z.ra_passwort, sizeof(g_z.ra_passwort),
                                   ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    Abstand(2);
    if (ImGui::Checkbox(T("Angemeldet bleiben"), &g_z.angemeldet_bleiben) && !g_z.angemeldet_bleiben) {
        g_z.gespeichertes_token.clear();
        EinstellungenSpeichern();
    }
    Abstand(4);
    if (theme::KnopfHaupt("Anmelden", g_z.laeuft == 0) || enter) Anmelden();
    Abstand(4);
    theme::Hinweis("Das Passwort wird nur für die Anmeldung verwendet, danach aus dem Feld gelöscht und nie "
                   "gespeichert. Mit \"Angemeldet bleiben\" wird nur das Sitzungstoken gespeichert.");
}

void SeiteNetz() {
    theme::Seitenkopf("Netz", "Verbindung zur Dreamcast über das Coder's Cable.");
    VerbindungsHinweis();
    theme::Feld("COM-Port des Kabels (leer = automatisch suchen)", "##kabel", g_z.kabel_port,
                sizeof(g_z.kabel_port), 0, "wird automatisch gefunden");
    Abstand(4);
    if (!g_sitzung.Laeuft()) {
        if (theme::KnopfHaupt("Verbinden")) KabelVerbinden();
    } else {
        if (theme::KnopfNeben("Trennen")) KabelTrennen();
    }
    Abstand(6);
    if (g_sitzung.Laeuft()) {
        theme::Marke(g_z.kabel_offen ? "Kabel geöffnet" : "Kabel nicht geöffnet",
                     g_z.kabel_offen ? Zustand::Gut : Zustand::Warnung);
        ImGui::SameLine();
        theme::Marke(g_z.im_menue ? "DC-RAW-Menü" : g_z.daten_kommen ? "Spiel läuft" : g_z.daten_aktuell ? "Dreamcast sendet" : "keine Daten",
                     (g_z.im_menue || g_z.daten_aktuell || g_z.daten_kommen) ? Zustand::Gut : Zustand::Neutral);
        Abstand(4);
        char t[64];

        if (g_z.bilder_pro_s == 0 && g_z.bereiche == 0 && g_z.spiel_id == 0)
            std::snprintf(t, sizeof(t), "kein RA-Spiel, keine Daten nötig");
        else
            std::snprintf(t, sizeof(t), "%u", g_z.bilder_pro_s);
        Zeile("Bilder/s", t, true);

        std::snprintf(t, sizeof(t), "%llu", static_cast<unsigned long long>(g_z.pruefsummenfehler));
        Zeile("Prüfsummen", t, true);
        std::snprintf(t, sizeof(t), "%llu", static_cast<unsigned long long>(g_z.bytes_empfangen));
        Zeile("Empfangen (Byte)", t, true);

    }

    Abstand(6);
    theme::Hinweis("Nach der Anmeldung sucht das Tool das Kabel selbst und verbindet sich. Dann an der Dreamcast im "
                   "DC-RAW-Menü das Spiel starten; es wird automatisch erkannt. Achievements zählen als Softcore.");
}

void LogZeilen(size_t ab) {
    ImGui::PushFont(theme::gMono, theme::kGroesseKlein);
    for (size_t i = ab; i < g_z.log.size(); ++i) {
        const Logzeile& z = g_z.log[i];
        ImGui::PushStyleColor(ImGuiCol_Text, F(theme::kSehrLeise));
        ImGui::TextUnformatted(z.zeit.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, F(z.art == Zustand::Neutral ? theme::kText
                                               : z.art == Zustand::Gut ? theme::kGut
                                               : z.art == Zustand::Warnung ? theme::kWarnung
                                               : z.art == Zustand::Fehler ? theme::kFehler
                                               : theme::kInfo));
        ImGui::TextWrapped("%s", z.text.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
}

void SeiteLog() {
    theme::Seitenkopf("Protokoll", nullptr);
    if (theme::KnopfNeben("Leeren")) g_z.log.clear();
    ImGui::SameLine();
    if (theme::KnopfNeben("Kopieren")) {
        std::string alles;
        for (const auto& z : g_z.log) alles += z.zeit + "  " + z.text + "\n";
        ImGui::SetClipboardText(alles.c_str());
    }
    Abstand(2);
    ImGui::BeginChild("##logliste", ImVec2(0, 0), 0);
    LogZeilen(0);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

char g_dr_laufwerk[8] = "";
char g_dr_platz[8] = "01";
std::string g_dr_fortschritt;
bool g_dr_aus_ersteinrichtung = false;
bool g_dr_nach_einrichten = false;
std::string g_dr_eingerichtet;

bool KarteAuswerfen(const std::string& lw, std::string& fehler) {
    const std::wstring pfad = L"\\\\.\\" + core::NachWide(lw);
    HANDLE h = ::CreateFileW(pfad.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { fehler = Fmt(T("Laufwerk %1 nicht gefunden"), {lw}); return false; }
    DWORD n = 0;
    bool gesperrt = false;
    for (int i = 0; i < 10 && !gesperrt; ++i) {
        gesperrt = ::DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &n, nullptr) != 0;
        if (!gesperrt) ::Sleep(200);
    }
    if (!gesperrt) {
        ::CloseHandle(h);
        fehler = T("Karte wird noch benutzt (Explorer-Fenster oder Programm schließen)");
        return false;
    }
    ::DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &n, nullptr);
    PREVENT_MEDIA_REMOVAL pmr{};
    pmr.PreventMediaRemoval = FALSE;
    ::DeviceIoControl(h, IOCTL_STORAGE_MEDIA_REMOVAL, &pmr, sizeof(pmr), nullptr, 0, &n, nullptr);
    const bool ok = ::DeviceIoControl(h, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &n, nullptr) != 0;
    ::CloseHandle(h);
    if (!ok) fehler = T("Auswerfen nicht möglich, bitte über Windows auswerfen");
    return ok;
}

void DreamRawSchreiben() {
    std::string lw = core::Getrimmt(g_dr_laufwerk);
    if (lw.size() == 1) lw += ":";
    const int platz = std::atoi(g_dr_platz);
    if (lw.size() != 2 || lw[1] != ':') { Log("Laufwerk bitte als Buchstabe mit Doppelpunkt, z. B. H:", Zustand::Warnung); return; }
    if (platz == 1)
        Log("Platz 01: DC-RAW ersetzt das Startmenü. Das bisherige Menü wird gesichert und lässt sich unter "
            "OPTIONEN zurückholen. CDI-Spiele startet DC-RAW noch nicht.", Zustand::Warnung);
    Log(Fmt(T("DC-RAW: richte Platz %1 auf %2 ein ..."), {std::to_string(platz), lw}));
    const std::wstring wurzel = core::NachWide(lw + "\\");
    const std::string benutzer = g_z.angemeldet ? g_z.benutzer : "";
    const std::string token = g_z.angemeldet ? g_z.token : "";
    g_dr_eingerichtet.clear();
    Nebenher([wurzel, platz, benutzer, token, lw]() -> std::function<void()> {
        const std::vector<uint8_t> prog(kDreamRawProgramm, kDreamRawProgramm + kDreamRawGroesse);
        core::StartlogoVorgeben(icons::StartlogoMr());
        const core::MenueErgebnis r = core::MenueAufKarte(
            wurzel, platz, prog, true,
            [benutzer, token](const std::vector<core::KartenPlatz>& p, std::vector<std::string>& m) {
                auto fortschritt = [](const std::string& s) { ImHauptthread([s]() { g_dr_fortschritt = T(s); }); };
                std::vector<core::GdiDatei> d = icons::IconsFuerMenue(p, benutzer, token, m, fortschritt);

                std::vector<std::pair<int, std::vector<core::Bereich>>> listen;
                int ohne = 0, zeiger_hinweis = 0;
                auto letzte = std::chrono::steady_clock::now() - std::chrono::seconds(5);
                for (const core::KartenPlatz& k : p) {
                    if (k.ra != '1' || !k.spiel_id) continue;

                    {
                        std::vector<core::Bereich> b;
                        if (core::AdresslisteLaden(k.spiel_id, b) && !b.empty()) {
                            listen.push_back({k.platz, b});
                            continue;
                        }
                    }
                    if (token.empty()) { ++ohne; continue; }
                    fortschritt(Fmt(T("Schritt 3/5: Spieldaten für Platz %1 ..."), {std::to_string(k.platz)}));
                    const auto seit = std::chrono::steady_clock::now() - letzte;
                    if (seit < std::chrono::milliseconds(1100))
                        std::this_thread::sleep_for(std::chrono::milliseconds(1100) - seit);
                    letzte = std::chrono::steady_clock::now();
                    const core::SpielDaten s = core::SpielLaden(benutzer, token, k.spiel_id);
                    std::vector<core::Bereich> b;
                    std::string grund;
                    if (s.ok) {
                        size_t n = 0;
                        b = core::AdresslisteAusSpiel(s, n);
                        if (!b.empty()) core::AdresslisteSpeichern(k.spiel_id, b);
                        for (const auto& a : s.achievements)
                            if (a.definition.find("I:") != std::string::npos) { ++zeiger_hinweis; break; }
                        size_t mit_def = 0;
                        for (const auto& a : s.achievements) if (!a.definition.empty()) ++mit_def;
                        if (b.empty())
                            grund = Fmt(T(s.rich_presence.empty()
                                              ? "%1 Achievements (%2 mit Bedingungen), %3 Leaderboards, keine Rich Presence, %4 Adressen ermittelt"
                                              : "%1 Achievements (%2 mit Bedingungen), %3 Leaderboards, mit Rich Presence, %4 Adressen ermittelt"),
                                        {std::to_string(s.achievements.size()), std::to_string(mit_def),
                                         std::to_string(s.leaderboards.size()), std::to_string(n)});
                    } else {
                        core::AdresslisteLaden(k.spiel_id, b);
                        if (b.empty()) grund = Fmt(T("RA-Abfrage fehlgeschlagen: %1"), {T(s.fehler)});
                    }
                    if (!b.empty()) listen.push_back({k.platz, b});
                    else if (s.ok && s.achievements.empty())
                        m.push_back(Fmt(T("Platz %1: Spiel %2 hat bei RA noch keine Achievements"),
                                        {std::to_string(k.platz), std::to_string(k.spiel_id)}));
                    else
                        m.push_back(Fmt(T("Platz %1: Spieldaten von RA fehlen, später erneut einrichten"),
                                        {std::to_string(k.platz)}));
                }
                if (ohne)
                    m.push_back(Fmt(T("Für %1 Spiele fehlen Daten von RA (zuerst bei RA anmelden)"), {std::to_string(ohne)}));
                if (!listen.empty()) d.push_back({"ADRESSEN.BIN", core::AdressenBinBauen(listen)});
                return d;
            },
            [](const std::string& s) {
                ImHauptthread([s]() {
                    g_dr_fortschritt = T(s);
                    Log("DC-RAW: " + T(s));
                });
            });
        return [r, lw]() {
            g_dr_fortschritt = r.ok ? std::string(T("Fertig, alle Dateien geprüft.")) : Fmt(T("Abgebrochen: %1"), {T(r.fehler)});
            if (r.ok) { g_dr_eingerichtet = lw; g_dr_nach_einrichten = g_dr_aus_ersteinrichtung; }
            g_dr_aus_ersteinrichtung = false;
            for (const std::string& m : r.meldungen) Log("DC-RAW: " + T(m), Zustand::Gut);
            if (!r.ok) Log(Fmt(T("DC-RAW nicht eingerichtet: %1"), {T(r.fehler)}), Zustand::Fehler);
            else Log("Fertig. Karte mit \"Karte auswerfen\" auswerfen und in die Dreamcast stecken.", Zustand::Gut);
        };
    });
}

constexpr int kJasonRessource = 300;
bilder::Textur g_jason, g_jason_gross, g_jason_plakette;
bool g_katze_oeffnen = false;

constexpr const char* kSeite = "https://liquid-wq.github.io/";
int g_update_build = 0;
bool g_update_oeffnen = false;
struct Nachricht { long long id = 0; std::string de, en, link; } g_nachricht;
bool g_nachricht_oeffnen = false;

bool JsonFeld(const std::string& j, const char* name, std::string* text, long long* zahl) {
    const std::string schluessel = std::string("\"") + name + "\"";
    size_t p = j.find(schluessel);
    if (p == std::string::npos) return false;
    p = j.find(':', p + schluessel.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) ++p;
    if (p >= j.size()) return false;
    if (j[p] == '"') {
        if (!text) return false;
        std::string s;
        for (++p; p < j.size() && j[p] != '"'; ++p) {
            char c = j[p];
            if (c != '\\') { s.push_back(c); continue; }
            if (++p >= j.size()) return false;
            switch (j[p]) {
            case 'n': s.push_back('\n'); break;
            case 't': s.push_back('\t'); break;
            case 'u': {
                if (p + 4 >= j.size()) return false;
                const unsigned cp = static_cast<unsigned>(std::strtoul(j.substr(p + 1, 4).c_str(), nullptr, 16));
                p += 4;
                if (cp < 0x80) s.push_back(static_cast<char>(cp));
                else if (cp < 0x800) { s.push_back(static_cast<char>(0xC0 | (cp >> 6))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                else { s.push_back(static_cast<char>(0xE0 | (cp >> 12))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                break;
            }
            default: s.push_back(j[p]); break;
            }
        }
        *text = s;
        return true;
    }
    if (!zahl) return false;
    char* ende = nullptr;
    const long long v = std::strtoll(j.c_str() + p, &ende, 10);
    if (ende == j.c_str() + p) return false;
    *zahl = v;
    return true;
}

void UpdatePruefen() {
    const long long gelesen = g_z.nachricht_gelesen;
    std::thread([gelesen]() {
        ::Sleep(2000);
        core::HttpAntwort a = core::HttpAnfrage(std::string(kSeite) + "data/version_dreamraw.txt", "", 8000);
        if (a.ok && a.status == 200) {

            const std::string& b = a.koerper;
            size_t s = 0;
            while (s < b.size() && static_cast<unsigned char>(b[s]) <= ' ') ++s;
            size_t e = s;
            while (e < b.size() && b[e] >= '0' && b[e] <= '9') ++e;
            size_t r = e;
            while (r < b.size() && static_cast<unsigned char>(b[r]) <= ' ') ++r;
            if (e > s && r == b.size()) {
                const int fern = std::atoi(b.substr(s, e - s).c_str());
                if (fern > buildinfo::kBuild)
                    ImHauptthread([fern]() { g_update_build = fern; g_update_oeffnen = true; });
            }
        }
        ::Sleep(500);
        core::HttpAntwort n = core::HttpAnfrage(std::string(kSeite) + "data/nachricht_dreamraw.json", "", 8000);
        if (!n.ok || n.status != 200) return;
        std::string j = n.koerper;
        if (j.size() >= 3 && static_cast<unsigned char>(j[0]) == 0xEF && static_cast<unsigned char>(j[1]) == 0xBB &&
            static_cast<unsigned char>(j[2]) == 0xBF)
            j.erase(0, 3);
        Nachricht m;
        long long ab = 0, bis = 1000000;
        if (!JsonFeld(j, "id", nullptr, &m.id) || m.id <= 0 || m.id == gelesen) return;
        JsonFeld(j, "ab_build", nullptr, &ab);
        JsonFeld(j, "bis_build", nullptr, &bis);
        if (buildinfo::kBuild < ab || buildinfo::kBuild > bis) return;
        JsonFeld(j, "de", &m.de, nullptr);
        JsonFeld(j, "en", &m.en, nullptr);
        JsonFeld(j, "link", &m.link, nullptr);
        if (m.link.rfind("https://", 0) != 0) m.link.clear();
        if (m.de.empty() && m.en.empty()) return;
        if (m.de.size() > 4000) m.de.resize(4000);
        if (m.en.size() > 4000) m.en.resize(4000);
        ImHauptthread([m]() { g_nachricht = m; g_nachricht_oeffnen = true; });
    }).detach();
}

void LinkOeffnen(const std::string& url) {
    const std::wstring w = core::NachWide(url);
    ::ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

struct DialogStil {
    DialogStil() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D(22), D(18)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, D(10));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kKartenrand));
    }
    ~DialogStil() {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(4);
    }
};

bool g_bereit_oeffnen = false;
void BereitFenster() {
    DialogStil stil;
    const char* id = T("DREAM-RAW ist bereit###bereit");
    if (g_bereit_oeffnen && !ImGui::IsPopupOpen(id, ImGuiPopupFlags_AnyPopupId)) { ImGui::OpenPopup(id); g_bereit_oeffnen = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(D(460), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + D(420));
    ImGui::TextUnformatted(T("Die SD-Karte ist eingerichtet und kann entnommen werden.\n\n"
                             "1. Karte zurück in den GDEMU stecken\n"
                             "2. Coder's Cable anschließen, Dreamcast einschalten\n"
                             "3. Im DC-RAW-Menü ein Spiel mit A starten\n\n"
                             "DREAM-RAW erkennt das Spiel von selbst."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, D(8)));
    if (theme::KnopfHaupt("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void UpdateFenster() {
    DialogStil stil;
    const char* id = T("Update verfügbar###update");
    if (g_update_oeffnen && !ImGui::IsPopupOpen(id, ImGuiPopupFlags_AnyPopupId)) { ImGui::OpenPopup(id); g_update_oeffnen = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(D(440), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(D(420));
    ImGui::TextUnformatted(Fmt(T("Eine neue Version ist verfügbar (Build %1, du hast Build %2)."),
                               {std::to_string(g_update_build), std::to_string(buildinfo::kBuild)}).c_str());
    ImGui::Dummy(ImVec2(0, D(4)));
    ImGui::TextUnformatted(T("Besuche die Projektseite zum Download."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, D(6)));
    if (ImGui::Button(T("Projektseite öffnen"))) { LinkOeffnen(std::string(kSeite) + "dream-raw/"); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button(T("Später"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void NachrichtFenster() {
    DialogStil stil;
    const char* id = T("Nachricht###nachricht");

    if (g_nachricht_oeffnen && !ImGui::IsPopupOpen(id, ImGuiPopupFlags_AnyPopupId) && !g_update_oeffnen) {
        ImGui::OpenPopup(id);
        g_nachricht_oeffnen = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(D(480), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) return;
    const std::string& text = (i18n::Englisch() && !g_nachricht.en.empty()) || g_nachricht.de.empty() ? g_nachricht.en : g_nachricht.de;
    ImGui::PushTextWrapPos(D(460));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, D(6)));
    auto gelesen = []() {
        g_z.nachricht_gelesen = g_nachricht.id;
        EinstellungenSpeichern();
        ImGui::CloseCurrentPopup();
    };
    if (!g_nachricht.link.empty() && ImGui::Button(T("Link öffnen"))) { LinkOeffnen(g_nachricht.link); gelesen(); }
    if (!g_nachricht.link.empty()) ImGui::SameLine();
    if (ImGui::Button("OK")) gelesen();
    ImGui::EndPopup();
}

void KatzeFenster() {
    DialogStil stil;
    const char* id = T("über die Katze###katze");
    if (g_katze_oeffnen) { ImGui::OpenPopup(id); g_katze_oeffnen = false; }
    ImGui::SetNextWindowSize(ImVec2(D(520), D(720)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool offen = true;
    if (!ImGui::BeginPopupModal(id, &offen, ImGuiWindowFlags_NoSavedSettings)) return;
    auto zentriert = [](const char* t, ImFont* f, float groesse, ImU32 farbe) {
        ImGui::PushFont(f, groesse);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(t).x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, farbe);
        ImGui::TextUnformatted(t);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    };
    if (g_jason_gross.ok()) {
        const float w = (float)g_jason_gross.w, h = (float)g_jason_gross.h;

        const float rand = D(14);
        ImGui::SetCursorPosX(std::floor(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - w) * 0.5f));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float kw = std::max(w + 2 * rand, D(300));
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + w * 0.5f - kw * 0.5f, p.y), ImVec2(p.x + w * 0.5f + kw * 0.5f, p.y + h + 2 * rand),
                                                  IM_COL32(0x2B, 0x2B, 0x2B, 0xFF), D(12));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rand));
        ImGui::Image((ImTextureID)(intptr_t)g_jason_gross.id, ImVec2(w, h), ImVec2(0, 0),
                     ImVec2(g_jason_gross.u, g_jason_gross.v));
        ImGui::Dummy(ImVec2(0, rand));
    }
    zentriert("In memory of Jason", theme::gHalbfett, theme::kGroesseZwisch, theme::kGut);
    zentriert("2010 \xE2\x80\x93 2025", theme::gNormal, theme::kGroesseKlein, IM_COL32(0x88, 0x88, 0x88, 0xFF));
    ImGui::Dummy(ImVec2(0, D(4)));
    const float unten = ImGui::GetFrameHeightWithSpacing() + D(6);
    ImGui::BeginChild("katzentext", ImVec2(0, ImGui::GetContentRegionAvail().y - unten),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushTextWrapPos(0.0f);
    {
        const std::string& sp = i18n::Sprache();
        const char* kt = sp == "de" ? kKatzeText : sp == "es" ? kKatzeTextEs : sp == "fr" ? kKatzeTextFr
                       : sp == "it" ? kKatzeTextIt : sp == "pl" ? kKatzeTextPl : sp == "pt" ? kKatzeTextPt : kKatzeTextEn;
        ImGui::TextUnformatted(kt);
    }
    ImGui::Dummy(ImVec2(0, D(6)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, D(4)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(T("Falls Sie kein Tier in Ihrer Familie haben: Auch die Tiere in den Tierheimen freuen sich, "
                             "denn sie sind ebenfalls auf Hilfe angewiesen."));
    {

        const char* satz = T("Und wer zusätzlich dieses Projekt unterstützen oder aus Wertschätzung etwas dalassen "
                             "möchte, kann das hier tun:");
        const char* wort = T("zusätzlich");
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::TextUnformatted(satz);
        const std::string st(satz);
        const size_t pos = st.find(wort);
        if (pos != std::string::npos) {
            const float x0 = ImGui::CalcTextSize(satz, satz + pos).x;
            const float x1 = x0 + ImGui::CalcTextSize(wort).x;
            if (x1 <= ImGui::GetContentRegionAvail().x) {
                const float y = p.y + ImGui::GetFontSize();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + x0, y), ImVec2(p.x + x1, y),
                                                    ImGui::GetColorU32(ImGuiCol_Text), D(1));
            }
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::TextLink("Ko-fi")) ::ShellExecuteW(nullptr, L"open", L"https://ko-fi.com/liqui69747", nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    if (ImGui::Button("OK", ImVec2(-FLT_MIN, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

bilder::Textur g_spiel_icon;
uint32_t g_spiel_icon_id = 0;
uint32_t g_spiel_icon_angefragt = 0;

void SpielIconAnfordern(uint32_t id, const std::string& url) {
    if (!id || g_spiel_icon_angefragt == id) return;
    g_spiel_icon_angefragt = id;
    Nebenher([id, url]() -> std::function<void()> {
        std::string png;
        std::wstring datei;
        if (const wchar_t* a = _wgetenv(L"APPDATA")) {
            const std::wstring o = std::wstring(a) + L"\\DREAM-RAW\\icon_cache";
            ::CreateDirectoryW((std::wstring(a) + L"\\DREAM-RAW").c_str(), nullptr);
            ::CreateDirectoryW(o.c_str(), nullptr);
            datei = o + L"\\" + std::to_wstring(id) + L".png";
            if (FILE* f = _wfopen(datei.c_str(), L"rb")) {
                char b[8192];
                size_t n;
                while ((n = std::fread(b, 1, sizeof(b), f)) > 0) png.append(b, n);
                std::fclose(f);
            }
        }
        if (png.empty() && !url.empty()) {
            const core::HttpAntwort h = core::HttpAnfrage(url, "", 8000);
            if (h.ok && h.status == 200 && h.koerper.size() > 8 && h.koerper.compare(1, 3, "PNG") == 0) {
                png = h.koerper;
                if (!datei.empty())
                    if (FILE* f = _wfopen(datei.c_str(), L"wb")) {
                        std::fwrite(png.data(), 1, png.size(), f);
                        std::fclose(f);
                    }
            }
        }
        return [id, png]() {
            if (png.empty()) return;
            bilder::Freigeben(g_spiel_icon);
            g_spiel_icon = bilder::PngHochladenSkaliert(png.data(), png.size(), static_cast<int>(std::lround(D(64))));
            g_spiel_icon_id = id;
        };
    });
}

volatile bool g_ra_abbruch = false;
bool g_ra_pruefung_laeuft = false;

void RaStatusPruefen() {
    std::string lw = core::Getrimmt(g_dr_laufwerk);
    if (lw.size() == 1) lw += ":";
    if (lw.size() != 2 || lw[1] != ':') { Log("Laufwerk bitte als Buchstabe mit Doppelpunkt, z. B. H:", Zustand::Warnung); return; }
    g_ra_abbruch = false;
    g_ra_pruefung_laeuft = true;
    Log(Fmt(T("RA-Status: prüfe die Karte %1 (höchstens eine RA-Abfrage pro Sekunde, Ergebnisse im Cache) ..."), {lw}));
    const std::wstring wurzel = core::NachWide(lw + "\\");
    Nebenher([wurzel]() -> std::function<void()> {
        std::vector<core::KartenPlatz> plaetze = core::KarteLesen(wurzel);
        const core::RaPruefung r = core::RaStatusBestimmen(plaetze, true, &g_ra_abbruch, [](const std::string& m) {
            ImHauptthread([m]() { g_dr_fortschritt = Fmt(T("RA-Status: %1"), {T(m)}); Log(g_dr_fortschritt); });
        });
        return [r]() {
            g_ra_pruefung_laeuft = false;
            const std::string zahlen = Fmt(T("%1 Spiele mit RA, %2 ohne, %3 ungeprüft, %4 Abfragen"),
                                           {std::to_string(r.bekannt), std::to_string(r.unbekannt),
                                            std::to_string(r.offen), std::to_string(r.abfragen)});
            if (!r.fehler.empty())
                Log(Fmt(T("RA-Status unterbrochen (%1). Später erneut starten, Erledigtes bleibt im Cache. %2"),
                        {T(r.fehler), zahlen}), Zustand::Warnung);
            else if (r.abgebrochen) Log(Fmt(T("RA-Status abgebrochen. %1"), {zahlen}), Zustand::Warnung);
            else Log(Fmt(T("RA-Prüfung fertig: %1. Jetzt \"Auf SD-Karte einrichten\"."), {zahlen}), Zustand::Gut);
        };
    });
}

void KarteSuchen() {
    const DWORD maske = ::GetLogicalDrives();
    for (int i = 3; i < 26; ++i) {
        if (!(maske & (1u << i))) continue;
        wchar_t w[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (::GetDriveTypeW(w) != DRIVE_REMOVABLE) continue;
        const std::wstring b = w;
        if (::GetFileAttributesW((b + L"GDEMU.ini").c_str()) == INVALID_FILE_ATTRIBUTES &&
            ::GetFileAttributesW((b + L"01").c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        g_dr_laufwerk[0] = static_cast<char>('A' + i);
        g_dr_laufwerk[1] = ':';
        g_dr_laufwerk[2] = 0;
        return;
    }
}

bool g_sicherung_da = false;
bool g_wh_gestartet = false;
void OriginalZurueck() {
    std::string lw = core::Getrimmt(g_dr_laufwerk);
    if (lw.size() == 1) lw += ":";
    const int platz = std::atoi(g_dr_platz);
    if (lw.size() != 2 || lw[1] != ':') return;
    const std::wstring wurzel = core::NachWide(lw + "\\");
    Log(Fmt(T("Stelle das ursprüngliche Menü auf Platz %1 (%2) wieder her ..."), {std::to_string(platz), lw}));
    g_dr_eingerichtet.clear();
    g_wh_gestartet = true;
    Nebenher([wurzel, platz, lw]() -> std::function<void()> {
        const core::MenueErgebnis r = core::OriginalWiederherstellen(wurzel, platz, [](const std::string& s) {
            ImHauptthread([s]() { g_dr_fortschritt = T(s); });
        });
        return [r, lw]() {
            g_dr_fortschritt = r.ok ? std::string(T("Fertig, alle Dateien geprüft.")) : Fmt(T("Abgebrochen: %1"), {T(r.fehler)});
            for (const std::string& m : r.meldungen) Log(T(m), Zustand::Gut);
            if (!r.ok) Log(Fmt(T("Nicht wiederhergestellt: %1"), {T(r.fehler)}), Zustand::Fehler);
            else { g_dr_eingerichtet = lw; g_sicherung_da = false; g_dr_nach_einrichten = false; }
        };
    });
}

void AuswerfenKnopf() {
    if (g_dr_eingerichtet.empty() || g_z.laeuft != 0) return;
    Abstand(2);
    if (theme::KnopfNeben(Fmt(T("Karte %1 auswerfen"), {g_dr_eingerichtet}).c_str())) {
        std::string fehler;
        if (KarteAuswerfen(g_dr_eingerichtet, fehler)) {
            Log(Fmt(T("Karte %1 ausgeworfen, kann entnommen werden."), {g_dr_eingerichtet}), Zustand::Gut);
            g_bereit_oeffnen = g_dr_nach_einrichten;
            g_dr_fortschritt = T("Karte ausgeworfen. Jetzt in die Dreamcast stecken.");
            g_dr_eingerichtet.clear();
        } else {
            Log(Fmt(T("Auswerfen: %1"), {fehler}), Zustand::Warnung);
            g_dr_fortschritt = fehler;
        }
    }
}

void SicherungPruefen() {
    std::string lw = core::Getrimmt(g_dr_laufwerk);
    if (lw.size() == 1) lw += ":";
    g_sicherung_da = lw.size() == 2 && core::SicherungVorhanden(core::NachWide(lw + "\\"), std::atoi(g_dr_platz));
}

void WiederherstellenKnopf() {
    DialogStil stil;
    if (!g_sicherung_da) {
        Leise("%s", "Ursprüngliches Menü wiederherstellen: keine Sicherung auf der Karte gefunden.");
    } else if (theme::KnopfNeben("Ursprüngliches Menü wiederherstellen", g_z.laeuft == 0)) {
        ImGui::OpenPopup(T("Wiederherstellen###wh"));
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Wiederherstellen###wh"), nullptr,
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(D(420));
        char nr[8];
        std::snprintf(nr, sizeof(nr), "%02d", std::atoi(g_dr_platz));
        ImGui::TextUnformatted(Fmt(T("Das Menü, das vor DC-RAW auf Platz %1 lag, wird zurückgespielt. DC-RAW wird "
                                     "von diesem Platz entfernt. Spurdateien, die DREAM-RAW umbenannt hat, bekommen wieder ihre ursprünglichen Namen."),
                                   {nr}).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, D(6)));
        if (ImGui::Button(T("Wiederherstellen"))) { ImGui::CloseCurrentPopup(); OriginalZurueck(); }
        ImGui::SameLine();
        if (ImGui::Button(T("Abbrechen"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_wh_gestartet && !g_dr_fortschritt.empty()) {
        Abstand(2);
        ImGui::TextWrapped("%s", g_dr_fortschritt.c_str());
    }
    AuswerfenKnopf();
}

void SeiteSetup() {
    theme::Seitenkopf("Setup", "DC-RAW auf der SD-Karte einrichten.");

    static double letzte_suche = -10.0;
    if (g_z.laeuft == 0 && ImGui::GetTime() - letzte_suche > 2.0 && !ImGui::IsAnyItemActive()) {
        letzte_suche = ImGui::GetTime();
        KarteSuchen();
    }
    theme::Feld("Laufwerk", "##dr_lw", g_dr_laufwerk, sizeof(g_dr_laufwerk), 0, "z. B. H:");
    theme::Feld("Platz (01 = Startmenü)", "##dr_platz", g_dr_platz, sizeof(g_dr_platz), 0, "01");
    Abstand(4);
    if (theme::KnopfHaupt("Auf SD-Karte einrichten", g_z.laeuft == 0)) DreamRawSchreiben();
    ImGui::SameLine();
    if (!g_ra_pruefung_laeuft) {
        if (theme::KnopfNeben("Spiele bei RA prüfen", g_z.laeuft == 0)) RaStatusPruefen();
    } else if (theme::KnopfNeben("Prüfung abbrechen")) {
        g_ra_abbruch = true;
    }
    if (!g_dr_fortschritt.empty()) {
        Abstand(2);
        ImGui::PushStyleColor(ImGuiCol_Text, F(g_z.laeuft ? theme::kInfo : theme::kText));
        ImGui::TextWrapped("%s", g_dr_fortschritt.c_str());
        ImGui::PopStyleColor();
    }
    AuswerfenKnopf();
    Abstand(4);
    theme::Hinweis("Richtet DC-RAW mit deiner Spieleliste auf der Karte ein. Was vorher auf dem Platz lag, "
                   "wird gesichert. Nach neuen Spielen auf der Karte einfach noch einmal einrichten.");
}

bool g_vorschau = false;
bool g_roentgen_vorschau = false;

void SeiteInfo() {
    theme::Seitenkopf("Optionen", nullptr);
    static double letzte_suche = -10.0;
    if (g_z.laeuft == 0 && ImGui::GetTime() - letzte_suche > 2.0) {
        letzte_suche = ImGui::GetTime();
        KarteSuchen();
        SicherungPruefen();
    }
    if (theme::KnopfHaupt("über die Katze")) g_katze_oeffnen = true;
    ImGui::SameLine();
    if (theme::KnopfNeben("Ko-fi")) g_katze_oeffnen = true;
    Abstand(4);

    {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, F(theme::kLeise));
        ImGui::TextUnformatted("Sprache / Language");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const auto& sp = i18n::Sprachen();
        ImGui::SetNextItemWidth(D(160));
        if (ImGui::BeginCombo("##sprache", sp[i18n::SpracheIndex()].name)) {
            for (const i18n::SpracheInfo& s : sp) {
                if (ImGui::Selectable(s.name, i18n::Sprache() == s.kennung)) {
                    i18n::SpracheSetzen(s.kennung);
                    g_z.sprache = s.kennung;
                    EinstellungenSpeichern();
                }
            }
            ImGui::EndCombo();
        }
    }
    Abstand(4);
    WiederherstellenKnopf();
    Abstand(4);
    if (theme::KnopfNeben("Fehler melden")) LinkOeffnen("https://github.com/liquid-wq/dream-raw/issues");
    Abstand(8);
    Zeile("Programm", "DREAM-RAW 1.0.0");
    Zeile("Stand", buildinfo::string());
}

bool g_erst_oeffnen = false;
int g_erst_schritt = 0;
void ErstEinrichtungFenster() {
    DialogStil stil;
    const char* id = "DREAM-RAW###erst";
    if (g_erst_oeffnen && !ImGui::IsPopupOpen(id)) { ImGui::OpenPopup(id); g_erst_oeffnen = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(D(500), 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_AlwaysAutoResize))
        return;
    auto knopf = [](const char* text, bool haupt = false) { return haupt ? theme::KnopfHaupt(text) : theme::KnopfNeben(text); };
    auto ueberschrift = [](const char* text) {
        ImGui::PushFont(theme::gHalbfett, theme::kGroesseZwisch);
        ImGui::TextUnformatted(T(text));
        ImGui::PopFont();
        Abstand(4);
    };
    auto absatz = [](const char* text) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + D(470));
        ImGui::TextUnformatted(T(text));
        ImGui::PopTextWrapPos();
    };
    auto fertig = [](const char* hinweis) {
        if (hinweis) Log(hinweis);
        ImGui::CloseCurrentPopup();
    };
    switch (g_erst_schritt) {
        case 0: {
            ImGui::TextUnformatted("Sprache / Language / Idioma / Langue / Lingua / J\xC4\x99zyk");
            Abstand(4);
            for (const i18n::SpracheInfo& s : i18n::Sprachen()) {
                if (ImGui::Selectable(s.name, i18n::Sprache() == s.kennung, 0, ImVec2(D(320), 0))) {
                    i18n::SpracheSetzen(s.kennung);
                    g_z.sprache = s.kennung;
                }
            }
            Abstand(8);
            if (knopf("Weiter", true) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                g_z.sprache = i18n::Sprache();
                EinstellungenSpeichern();
                g_erst_schritt = 1;
            }
            break;
        }
        case 1:
            ueberschrift("Willkommen bei DREAM-RAW");
            absatz("Mit DREAM-RAW sammelst du RetroAchievements auf einer echten Dreamcast mit GDEMU.\n\n"
                   "Du brauchst:\n"
                   "• ein Coder's Cable (USB-Kabel für den seriellen Anschluss der Dreamcast)\n"
                   "• dein RetroAchievements-Konto\n"
                   "• die SD-Karte deines GDEMU und einen Kartenleser\n\n"
                   "Die Einrichtung hat zwei Schritte: anmelden, dann DC-RAW auf die SD-Karte schreiben.");
            Abstand(8);
            if (knopf("Weiter", true)) g_erst_schritt = g_z.angemeldet ? 3 : 2;
            ImGui::SameLine();
            if (knopf("Später")) fertig("Die Einrichtung geht jederzeit über B · Konto und SETUP.");
            break;
        case 2: {
            ueberschrift("Schritt 1 von 2: Anmelden");
            if (g_z.angemeldet) {
                theme::Marke("angemeldet", Zustand::Gut);
                Abstand(2);
                ImGui::TextUnformatted(Fmt(T("Angemeldet als %1."), {g_z.benutzer}).c_str());
                Abstand(8);
                if (knopf("Weiter", true)) g_erst_schritt = 3;
                break;
            }
            absatz("Mit deinem RetroAchievements-Konto anmelden. Das Passwort wird nicht gespeichert.");
            Abstand(4);
            theme::Feld("Benutzername", "##erst_user", g_z.ra_benutzer, sizeof(g_z.ra_benutzer));
            const bool enter = theme::Feld("Passwort", "##erst_pass", g_z.ra_passwort, sizeof(g_z.ra_passwort),
                                           ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Checkbox(T("Angemeldet bleiben"), &g_z.angemeldet_bleiben);
            Abstand(6);
            if (knopf("Anmelden", true) || enter) { if (g_z.laeuft == 0) Anmelden(); }
            ImGui::SameLine();
            if (knopf("Überspringen")) g_erst_schritt = 3;
            if (g_z.laeuft) { Abstand(2); Leise("%s", "Anmeldung läuft ..."); }
            else if (!g_z.log.empty() && g_z.log.back().art == Zustand::Fehler) {
                Abstand(2);
                ImGui::PushStyleColor(ImGuiCol_Text, F(theme::kFehler));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + D(470));
                ImGui::TextUnformatted(g_z.log.back().text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            break;
        }
        default: {
            ueberschrift("Schritt 2 von 2: SD-Karte einrichten");
            absatz("DC-RAW ist das Spielemenü auf der Dreamcast, das deine Spiele mit RetroAchievements startet. "
                   "Es kommt auf Platz 01 der SD-Karte; was dort liegt, wird gesichert und lässt sich unter OPTIONEN "
                   "zurückholen.\n\n"
                   "1. Dreamcast ausschalten\n"
                   "2. SD-Karte des GDEMU in den Kartenleser stecken\n"
                   "3. „Jetzt einrichten“ klicken");
            static double letzte = -10.0;
            if (ImGui::GetTime() - letzte > 2.0) { letzte = ImGui::GetTime(); KarteSuchen(); }
            const bool karte = ::GetDriveTypeA((std::string(g_dr_laufwerk) + "\\").c_str()) == DRIVE_REMOVABLE &&
                               ::GetFileAttributesA((std::string(g_dr_laufwerk) + "\\GDEMU.ini").c_str()) !=
                                   INVALID_FILE_ATTRIBUTES;
            Abstand(6);
            if (karte) theme::Marke(Fmt(T("SD-Karte gefunden: %1"), {g_dr_laufwerk}).c_str(), Zustand::Gut);
            else theme::Marke("SD-Karte des GDEMU noch nicht gefunden", Zustand::Neutral);
            Abstand(8);
            if (theme::KnopfHaupt("Jetzt einrichten", karte && g_z.laeuft == 0)) {
                std::snprintf(g_dr_platz, sizeof(g_dr_platz), "01");
                g_z.seite = Seite::Setup;
                g_dr_aus_ersteinrichtung = true;
                DreamRawSchreiben();
                fertig(nullptr);
            }
            ImGui::SameLine();
            if (knopf("Später")) fertig("Die Einrichtung geht jederzeit über SETUP → „Auf SD-Karte einrichten“.");
            break;
        }
    }
    ImGui::EndPopup();
}

struct Leuchte {
    const char* name;
    bool an;
};

constexpr ImU32 kRille = IM_COL32(0x8E, 0x8D, 0x86, 0x70);
constexpr ImU32 kRilleLicht = IM_COL32(0xFF, 0xFF, 0xFF, 0xC0);

constexpr ImU32 kHoch = IM_COL32(0xE2, 0xE1, 0xDC, 0xFF);

template <typename Abb>
void SchattenKnick(ImDrawList* dl, ImVec2 p, ImVec2 q, ImVec2 n, Abb abb) {
    for (int i = 0; i < 6; ++i) {
        const float o = (1.5f + i * 2.5f) * g_dpi;
        dl->AddLine(abb(ImVec2(p.x + n.x * o, p.y + n.y * o)), abb(ImVec2(q.x + n.x * o, q.y + n.y * o)),
                    IM_COL32(0x80, 0x7F, 0x78, 0x08), 2.5f * g_dpi);
    }
    const float l = 1.0f * g_dpi;
    dl->AddLine(abb(ImVec2(p.x - n.x * l, p.y - n.y * l)), abb(ImVec2(q.x - n.x * l, q.y - n.y * l)),
                kRilleLicht, 1.0f * g_dpi);
}

void VerlaufViereck(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3,
                    ImU32 c0, ImU32 c1, ImU32 c2, ImU32 c3) {
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(6, 4);
    const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteIdx(i0); dl->PrimWriteIdx((ImDrawIdx)(i0 + 1)); dl->PrimWriteIdx((ImDrawIdx)(i0 + 2));
    dl->PrimWriteIdx(i0); dl->PrimWriteIdx((ImDrawIdx)(i0 + 2)); dl->PrimWriteIdx((ImDrawIdx)(i0 + 3));
    dl->PrimWriteVtx(p0, uv, c0);
    dl->PrimWriteVtx(p1, uv, c1);
    dl->PrimWriteVtx(p2, uv, c2);
    dl->PrimWriteVtx(p3, uv, c3);
}

void Fugenlinie(ImDrawList* dl, ImVec2 p, ImVec2 q) {
    dl->AddLine(p, q, kRille, 1.2f * g_dpi);
    dl->AddLine(ImVec2(p.x + 1.2f * g_dpi, p.y + 1.0f), ImVec2(q.x + 1.2f * g_dpi, q.y + 1.0f), kRilleLicht,
                1.0f * g_dpi);
}

void Leuchten(ImDrawList* dl, ImVec2 m, float rb, bool links, const Leuchte* l, int n) {

    const float pi = 3.14159265f;
    const float h = D(4.5f);
    const ImVec2 maus = ImGui::GetIO().MousePos;
    for (int i = 0; i < n; ++i) {
        const float t = (i - (n - 1) * 0.5f) * 0.16f;
        const float w = links ? pi - t : t;
        const ImVec2 p(m.x + std::cos(w) * rb, m.y + std::sin(w) * rb);
        const ImVec2 ra(std::cos(w), std::sin(w));
        const ImVec2 ta(-ra.y, ra.x);
        auto ecke = [&](float u, float v) {
            return ImVec2(p.x + (ra.x * u + ta.x * v) * h, p.y + (ra.y * u + ta.y * v) * h);
        };
        ImVec2 q[4] = {ecke(-1, -1), ecke(1, -1), ecke(1, 1), ecke(-1, 1)};

        const float kreuz = (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[1].y - q[0].y) * (q[2].x - q[0].x);
        if (kreuz < 0) std::swap(q[1], q[3]);

        if (l[i].an) {
            dl->AddCircleFilled(p, h * 2.6f, IM_COL32(0xE4, 0x55, 0x2C, 0x30), 20);
            dl->AddConvexPolyFilled(q, 4, theme::kAkzent);
        } else {
            dl->AddConvexPolyFilled(q, 4, IM_COL32(0xEC, 0xEB, 0xE6, 0xFF));
        }

        for (int k = 0; k < 4; ++k) {
            const ImVec2 e0 = q[k], e1 = q[(k + 1) % 4];
            const ImVec2 mitte((e0.x + e1.x) * 0.5f - p.x, (e0.y + e1.y) * 0.5f - p.y);
            const bool licht = (mitte.x + mitte.y) < 0.0f;
            const ImU32 c = l[i].an ? (licht ? IM_COL32(0xFF, 0xC4, 0xA8, 0xFF) : theme::kAkzentDunkel)
                                    : (licht ? IM_COL32(0xFF, 0xFF, 0xFF, 0xFF) : IM_COL32(0xA8, 0xA7, 0xA0, 0xFF));
            dl->AddLine(e0, e1, c, 1.2f * g_dpi);
        }
        const float dx = maus.x - p.x, dy = maus.y - p.y;
        if (dx * dx + dy * dy < h * h * 6.0f)
            ImGui::SetTooltip("%s: %s", T(l[i].name), T(l[i].an ? "ja" : "noch nicht"));
    }
}

void FluegelUeberschrift(const char* text) {
    text = T(text);
    ImGui::PushFont(theme::gHalbfett, theme::kGroesseKlein);
    ImGui::PushStyleColor(ImGuiCol_Text, F(theme::kSehrLeise));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

float g_deckel = 0.0f;
bool DcVerbunden() { return g_z.daten_kommen; }

double g_deckel_zu_seit = -1.0;

constexpr float kAuf = 1.0f, kFlug = 1.3f, kZu = 1.8f, kRoe = 1.4f;

bool SeqLaeuft() { return g_seq_start >= 0.0; }
float SeqZeit() { return (float)(ImGui::GetTime() - g_seq_start); }
void SeqStarten() {

    g_seq_start = ImGui::GetTime() + (g_deckel < 0.5f ? kAuf : 0.0f);
}

float Glatt(float x) {
    x = std::fmax(0.0f, std::fmin(1.0f, x));
    return x * x * (3.0f - 2.0f * x);
}

void DeckelBewegen() {
    if (SeqLaeuft()) {
        const float t = SeqZeit();
        g_deckel = t < 0 ? Glatt((t + kAuf) / kAuf) : t < kFlug ? 1.0f : 1.0f - Glatt((t - kFlug) / kZu);

        if (!g_seq_vorschau && (t > kFlug + kZu + kRoe + 0.3f || !g_sitzung.Laeuft())) g_seq_start = -1.0;
    } else {

        const float ziel = g_vorschau ? 1.0f : 0.0f;
        const float dt = ImGui::GetIO().DeltaTime;
        g_deckel += (ziel - g_deckel) * (1.0f - std::exp(-dt * 4.5f));
        if (std::fabs(ziel - g_deckel) < 0.002f) g_deckel = ziel;
    }
    if (g_deckel < 0.02f) { if (g_deckel_zu_seit < 0) g_deckel_zu_seit = ImGui::GetTime(); }
    else g_deckel_zu_seit = -1.0;
}

void BogenText(ImDrawList* dl, ImFont* f, float groesse, ImVec2 m, float radius, float mitte_winkel,
               const char* text, ImU32 farbe) {
    if (!text || !*text || radius <= 1.0f) return;
    float breite = f->CalcTextSizeA(groesse, FLT_MAX, 0.0f, text).x;
    const float max_bogen = 2.5f;
    if (breite / radius > max_bogen) {
        groesse *= max_bogen * radius / breite;
        breite = max_bogen * radius;
    }
    float w = mitte_winkel - breite / radius * 0.5f;
    const char* s = text;
    while (*s) {

        const unsigned char b0 = static_cast<unsigned char>(*s);
        const int n = b0 < 0x80 ? 1 : (b0 >> 5) == 0x6 ? 2 : (b0 >> 4) == 0xE ? 3 : 4;
        char zeichen[8] = {};
        int k = 0;
        for (; k < n && s[k]; ++k) zeichen[k] = s[k];
        s += k;
        const float adv = f->CalcTextSizeA(groesse, FLT_MAX, 0.0f, zeichen).x;
        const float wm = w + adv * 0.5f / radius;
        const ImVec2 p(m.x + std::cos(wm) * radius, m.y + std::sin(wm) * radius);
        const int start = dl->VtxBuffer.Size;
        dl->AddText(f, groesse, ImVec2(p.x - adv * 0.5f, p.y - groesse * 0.5f), farbe, zeichen);
        const float rot = wm + 1.5707963f, cs = std::cos(rot), sn = std::sin(rot);
        for (int i = start; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            const float dx = v.pos.x - p.x, dy = v.pos.y - p.y;
            v.pos = ImVec2(p.x + dx * cs - dy * sn, p.y + dx * sn + dy * cs);
        }
        w += adv / radius;
    }
}

struct FrontMasse {
    float breite;
    float trapez_oben;
    float trapez_y;
    float leuchte_spitze;
};
FrontMasse FrontMasseBerechnen(ImVec2 m, float R) {
    FrontMasse f;
    f.breite = R * 0.085f;
    f.trapez_oben = f.breite * 1.6f;
    f.trapez_y = m.y + R - f.breite * 1.7f;
    f.leuchte_spitze = m.y + R + f.breite * 1.7f;
    return f;
}

const char* CdTitel() {
    if (g_z.spiel.ok && !g_z.spiel.titel.empty()) return g_z.spiel.titel.c_str();

    if (!g_z.dc_titel.empty() && !g_z.im_menue) return g_z.dc_titel.c_str();
    return "";
}

void ScheibeZeichnen(ImDrawList* dl, ImVec2 m, float rd, float dreh, float a) {
    const float pi2 = 6.2831853f;
    auto A = [](ImU32 c, float al) {
        return (c & 0x00FFFFFF) | ((ImU32)(((c >> 24) & 0xFF) * std::fmax(0.0f, std::fmin(1.0f, al))) << 24);
    };
    dl->AddCircleFilled(m, rd, A(IM_COL32(0xB4, 0xB8, 0xC0, 0xFF), a), 120);
    for (int i = 0; i < 48; ++i) {
        const float w0 = dreh + i * pi2 / 48.0f, w1 = w0 + pi2 / 48.0f;
        const float farbton = std::fmod(i / 48.0f * 2.0f, 1.0f);
        float rr, gg, bb;
        ImGui::ColorConvertHSVtoRGB(farbton, 0.35f, 1.0f, rr, gg, bb);
        const float hell = 0.5f + 0.5f * std::cos((w0 - dreh) * 2.0f);
        const ImU32 c = IM_COL32((int)(rr * 255), (int)(gg * 255), (int)(bb * 255), (int)(110 * hell * a));
        dl->PathClear();
        dl->PathLineTo(ImVec2(m.x + std::cos(w0) * rd * 0.30f, m.y + std::sin(w0) * rd * 0.30f));
        dl->PathArcTo(m, rd, w0, w1, 4);
        dl->PathLineTo(ImVec2(m.x + std::cos(w1) * rd * 0.30f, m.y + std::sin(w1) * rd * 0.30f));
        dl->PathFillConvex(c);
    }
    dl->AddCircle(m, rd, A(IM_COL32(0xF0, 0xF2, 0xF5, 0xFF), a), 120, D(1.0f));
    dl->AddCircleFilled(m, rd * 0.30f, A(IM_COL32(0xA6, 0xAA, 0xB2, 0xFF), a), 48);
    dl->AddCircle(m, rd * 0.30f, A(IM_COL32(0xFF, 0xFF, 0xFF, 0xFF), a * 0.5f), 48, 1.0f);
    dl->AddCircleFilled(m, rd * 0.12f, A(IM_COL32(0x2C, 0x30, 0x36, 0xFF), a), 32);
    dl->AddCircleFilled(m, rd * 0.05f, A(IM_COL32(0x8E, 0x93, 0x9A, 0xFF), a), 16);

    ImFont* f = theme::gHalbfett ? theme::gHalbfett : ImGui::GetFont();
    const float g = std::fmax(9.0f, rd * 0.13f);
    const float phase = std::fmod(dreh, pi2);
    const float glut = std::pow(std::fmax(0.0f, std::cos(phase)), 14.0f);
    const float winkel = -1.5707963f + dreh;
    if (glut > 0.01f) {
        const ImU32 schein = A(IM_COL32(0xFF, 0xF2, 0xD8, 0xFF), 0.35f * glut * a);
        const float o = std::fmax(1.0f, D(1.2f));
        BogenText(dl, f, g, ImVec2(m.x - o, m.y), rd * 0.62f, winkel, CdTitel(), schein);
        BogenText(dl, f, g, ImVec2(m.x + o, m.y), rd * 0.62f, winkel, CdTitel(), schein);
        BogenText(dl, f, g, ImVec2(m.x, m.y - o), rd * 0.62f, winkel, CdTitel(), schein);
        BogenText(dl, f, g, ImVec2(m.x, m.y + o), rd * 0.62f, winkel, CdTitel(), schein);
    }
    auto misch = [](ImU32 x, ImU32 y, float t) {
        auto k = [&](int s) {
            const float p = (float)((x >> s) & 0xFF), q = (float)((y >> s) & 0xFF);
            return (ImU32)(p + (q - p) * t) << s;
        };
        return k(0) | k(8) | k(16) | k(24);
    };
    const ImU32 farbe = misch(IM_COL32(0x26, 0x2A, 0x32, 0xFF), IM_COL32(0xFF, 0x7A, 0x1E, 0xFF), glut);
    BogenText(dl, f, g, m, rd * 0.62f, winkel, CdTitel(), A(farbe, (0.85f + 0.15f * glut) * a));
}

void FlugScheibe(ImDrawList* dl, ImVec2 m, float R, float rd) {
    if (!SeqLaeuft()) return;
    const float t = SeqZeit();
    if (t < 0 || t >= kFlug) return;
    const float x = t / kFlug;
    const float u = 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
    const ImVec2 p0(m.x + R * 1.75f, m.y - R * 1.30f), c(m.x + R * 1.05f, m.y - R * 1.75f), p1 = m;
    const float v = 1.0f - u;
    const ImVec2 p(v * v * p0.x + 2 * v * u * c.x + u * u * p1.x, v * v * p0.y + 2 * v * u * c.y + u * u * p1.y);
    const float groesse = rd * (1.45f - 0.45f * u);
    const float alpha = std::fmin(1.0f, x / 0.12f);

    dl->AddCircleFilled(ImVec2(m.x + D(6) * v, m.y + D(10) * v), rd * (0.55f + 0.45f * u),
                        IM_COL32(0x00, 0x00, 0x00, (int)(90 * u * alpha)), 64);

    ScheibeZeichnen(dl, p, groesse, ((float)ImGui::GetTime() + (kFlug - t)) * 0.9f, alpha);
}

float g_roentgen = 0.0f;

bool RoentgenAktiv() {

    if (SeqLaeuft()) return SeqZeit() >= kFlug + kZu;
    if (g_deckel >= 0.02f || g_deckel_zu_seit < 0 || ImGui::GetTime() - g_deckel_zu_seit < 0.35) return false;
    if (g_roentgen_vorschau) return true;
    return g_z.seite == Seite::Spiel && g_z.zeige_ram && g_sitzung.Laeuft() && DcVerbunden();
}

namespace durchsicht {

ImU32 A(ImU32 c, float a) {
    const int al = (int)(((c >> 24) & 0xFF) * std::fmax(0.0f, std::fmin(1.0f, a)));
    return (c & 0x00FFFFFF) | ((ImU32)al << 24);
}

ImU32 Mische(ImU32 x, ImU32 y, float t) {
    t = std::fmax(0.0f, std::fmin(1.0f, t));
    auto k = [&](int s) {
        const float a = (float)((x >> s) & 0xFF), b = (float)((y >> s) & 0xFF);
        return (ImU32)(a + (b - a) * t) << s;
    };
    return k(0) | k(8) | k(16) | k(24);
}

float Sehne(ImVec2 m, float R, float y) {
    const float d = y - m.y;
    return d * d >= R * R ? 0.0f : std::sqrt(R * R - d * d);
}

void RechteckImKreis(ImDrawList* dl, ImVec2 m, float R, ImVec2 a, ImVec2 b, ImU32 c) {
    const float schritt = 2.0f;
    for (float y = a.y; y < b.y; y += schritt) {
        const float h = Sehne(m, R, y + schritt * 0.5f);
        const float x0 = std::fmax(a.x, m.x - h), x1 = std::fmin(b.x, m.x + h);
        if (x1 > x0) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, std::fmin(b.y, y + schritt)), c);
    }
}

struct Bahn { float x0, y0, x1, y1; };
const Bahn kBahnen[] = {
    {-0.95f, -0.20f, -0.40f, -0.20f}, {-0.40f, -0.20f, -0.40f, 0.05f}, {-0.90f, 0.30f, -0.55f, 0.30f},
    {0.95f, -0.25f, 0.45f, -0.25f},   {0.45f, -0.25f, 0.45f, 0.02f},   {0.90f, 0.35f, 0.55f, 0.35f},
    {-0.70f, 0.62f, 0.70f, 0.62f},    {-0.30f, -0.80f, -0.30f, -0.55f}, {0.30f, -0.80f, 0.30f, -0.55f},
    {-0.62f, -0.55f, 0.62f, -0.55f},  {-0.85f, 0.10f, -0.62f, 0.10f},   {0.85f, 0.12f, 0.62f, 0.12f},
};
struct Teil { float x, y; bool hell; };
const Teil kTeile[] = {
    {-0.80f, -0.35f, true}, {-0.72f, -0.35f, false}, {0.78f, -0.40f, true}, {0.70f, -0.40f, false},
    {-0.86f, 0.45f, false}, {0.84f, 0.48f, true},    {-0.15f, -0.70f, true}, {0.15f, -0.70f, false},
    {-0.55f, 0.75f, true},  {0.55f, 0.76f, false},   {0.0f, 0.80f, true},    {-0.92f, 0.00f, true},
};

}

struct HuellenForm {
    ImVec2 knick_l, knick_r, rand_l, rand_r;
    float vorderkante = 0;
};

void RoentgenBlick(ImDrawList* dl, ImVec2 m, float R, float r, const HuellenForm& form) {
    using namespace durchsicht;
    const float a = g_roentgen;
    if (a <= 0.001f) return;
    const float t = (float)ImGui::GetTime();
    const float pi2 = 6.2831853f;

    dl->AddCircleFilled(m, R, A(IM_COL32(0x10, 0x24, 0x1A, 0xFF), a), 180);

    for (float y = m.y - R + D(9); y < m.y + R; y += D(13)) {
        const float h = Sehne(m, R, y);
        if (h > 0) dl->AddLine(ImVec2(m.x - h, y), ImVec2(m.x + h, y), A(IM_COL32(0x2A, 0x5E, 0x3C, 0x60), a), 1.0f);
    }

    for (const Bahn& b : kBahnen) {
        const ImVec2 p(m.x + b.x0 * R, m.y + b.y0 * R), q(m.x + b.x1 * R, m.y + b.y1 * R);
        dl->AddLine(p, q, A(IM_COL32(0x3C, 0x7A, 0x4E, 0xC0), a), D(2.2f));
        dl->AddCircleFilled(p, D(3.2f), A(IM_COL32(0xC9, 0xA8, 0x55, 0xE0), a), 12);
        dl->AddCircleFilled(q, D(3.2f), A(IM_COL32(0xC9, 0xA8, 0x55, 0xE0), a), 12);
    }

    for (const Teil& k : kTeile) {
        const ImVec2 c(m.x + k.x * R, m.y + k.y * R);
        const ImU32 farbe = k.hell ? IM_COL32(0xB8, 0x9B, 0x6E, 0xFF) : IM_COL32(0x22, 0x22, 0x24, 0xFF);
        dl->AddRectFilled(ImVec2(c.x - D(5), c.y - D(2.5f)), ImVec2(c.x + D(5), c.y + D(2.5f)), A(farbe, a), D(1));
        dl->AddRectFilled(ImVec2(c.x - D(5), c.y - D(2.5f)), ImVec2(c.x - D(3.5f), c.y + D(2.5f)),
                          A(IM_COL32(0xD8, 0xD8, 0xD8, 0xFF), a));
        dl->AddRectFilled(ImVec2(c.x + D(3.5f), c.y - D(2.5f)), ImVec2(c.x + D(5), c.y + D(2.5f)),
                          A(IM_COL32(0xD8, 0xD8, 0xD8, 0xFF), a));
    }

    const float rd = r * 0.92f;
    dl->AddCircleFilled(m, rd * 1.04f, A(IM_COL32(0x1A, 0x1C, 0x20, 0xFF), a * 0.85f), 120);
    ScheibeZeichnen(dl, m, rd, t * 0.9f, a * 0.85f);

    const size_t anzahl = g_z.ram.size();
    const float oben = m.y - r * 0.42f, unten = m.y + r * 0.58f;
    const float breite = r * 1.62f, spalt = D(16);
    const float cb = (breite - spalt) * 0.5f, ch = unten - oben;
    const double jetzt = ImGui::GetTime();
    for (int chip = 0; chip < 2; ++chip) {
        const float cx = m.x - breite * 0.5f + chip * (cb + spalt);
        const ImVec2 c0(cx, oben), c1(cx + cb, unten);

        dl->AddRectFilled(ImVec2(c0.x + D(3), c0.y + D(4)), ImVec2(c1.x + D(3), c1.y + D(4)),
                          A(IM_COL32(0x00, 0x00, 0x00, 0x60), a), D(3));
        for (int p = 0; p < 14; ++p) {
            const float py = oben + D(8) + p * (ch - D(16)) / 13.0f;
            dl->AddRectFilled(ImVec2(c0.x - D(7), py - D(1.6f)), ImVec2(c0.x, py + D(1.6f)),
                              A(IM_COL32(0xC4, 0xC8, 0xCE, 0xFF), a));
            dl->AddRectFilled(ImVec2(c1.x, py - D(1.6f)), ImVec2(c1.x + D(7), py + D(1.6f)),
                              A(IM_COL32(0xC4, 0xC8, 0xCE, 0xFF), a));
        }
        dl->AddRectFilledMultiColor(c0, c1, A(IM_COL32(0x2A, 0x2B, 0x2F, 0xFF), a), A(IM_COL32(0x22, 0x23, 0x27, 0xFF), a),
                                    A(IM_COL32(0x16, 0x17, 0x1A, 0xFF), a), A(IM_COL32(0x1C, 0x1D, 0x20, 0xFF), a));
        dl->AddRect(c0, c1, A(IM_COL32(0x45, 0x47, 0x4D, 0xFF), a), D(2), 0, 1.0f);
        dl->AddCircleFilled(ImVec2(cx + D(8), oben + D(8)), D(2.6f), A(IM_COL32(0x0E, 0x0E, 0x10, 0xFF), a), 12);
        char aufdruck[24];
        std::snprintf(aufdruck, sizeof(aufdruck), "SDRAM %d  8MB", chip);
        dl->AddText(theme::gMono, theme::kGroesseKlein - 2.0f, ImVec2(cx + D(16), oben + D(3)),
                    A(IM_COL32(0xA8, 0xAA, 0xB0, 0xFF), a), aufdruck);

        const float zo = oben + D(17), zb = (cb - D(8)) / 4.0f, zh = (ch - D(21)) / 8.0f;
        for (int z = 0; z < 32; ++z) {
            const size_t idx = (size_t)chip * 32 + z;
            if (idx >= anzahl) break;
            const core::RamZeile& e = g_z.ram[idx];
            const float zx = cx + D(4) + (z % 4) * zb, zy = zo + (z / 4) * zh;
            float glut = 0.0f;
            auto it = g_z.geaendert_um.find(e.adresse);
            if (it != g_z.geaendert_um.end()) glut = std::fmax(0.0f, 1.0f - (float)(jetzt - it->second) / 0.9f);
            const ImU32 weiss = IM_COL32(0xE6, 0xE6, 0xE8, 0xFF), orange = IM_COL32(0xFF, 0x96, 0x32, 0xFF);
            if (glut > 0)
                dl->AddCircleFilled(ImVec2(zx + zb * 0.5f, zy + zh * 0.40f), zb * 0.40f,
                                    IM_COL32(0xFF, 0x7A, 0x20, (int)(80 * glut * a)), 20);
            char wert[8], adr[12];
            std::snprintf(wert, sizeof(wert), "%02X", e.wert);
            std::snprintf(adr, sizeof(adr), "%06X", e.adresse);
            const ImU32 wf = !e.empfangen ? A(weiss, 0.30f * a) : A(Mische(weiss, orange, glut), a);
            const float wg = std::fmin(theme::kGroesseZwisch, zh * 0.62f / g_dpi * 1.0f) * 1.0f;
            const ImVec2 wgr = theme::gMono->CalcTextSizeA(wg, FLT_MAX, 0, wert);
            dl->AddText(theme::gMono, wg, ImVec2(zx + (zb - wgr.x) * 0.5f, zy), wf, wert);
            const float ag = std::fmax(8.0f, theme::kGroesseKlein - 3.5f);
            const ImVec2 agr = theme::gMono->CalcTextSizeA(ag, FLT_MAX, 0, adr);
            if (agr.x < zb)
                dl->AddText(theme::gMono, ag, ImVec2(zx + (zb - agr.x) * 0.5f, zy + wgr.y - D(2)),
                            A(IM_COL32(0x8C, 0x8E, 0x94, 0xFF), a), adr);
        }
    }

    {
        dl->PushClipRect(ImVec2(m.x - breite * 0.5f - D(10), oben - D(6)), ImVec2(m.x + breite * 0.5f + D(10), unten + D(8)),
                         true);
        ScheibeZeichnen(dl, m, rd, t * 0.9f, a * 0.32f);

        for (int i = 0; i < 14; ++i) {
            const float u = i / 13.0f;
            const float d = -rd * 0.55f + u * rd * 0.40f;
            const float h = std::sqrt(std::fmax(0.0f, rd * rd - d * d));
            const ImVec2 c(m.x + d * 0.7071f, m.y + d * 0.7071f);
            dl->AddLine(ImVec2(c.x - 0.7071f * h, c.y + 0.7071f * h), ImVec2(c.x + 0.7071f * h, c.y - 0.7071f * h),
                        A(IM_COL32(0xFF, 0xFF, 0xFF, 0xFF), 0.06f * std::sin(u * 3.14159f) * a), rd * 0.40f / 13.0f + 1.0f);
        }
        dl->PopClipRect();
    }

    {
        const ImU32 orange = IM_COL32(0xFF, 0x8A, 0x2A, 0xFF);
        const bool aktiv = g_z.daten_kommen || g_seq_vorschau || g_roentgen_vorschau;
        for (int chip = 0; chip < 2; ++chip) {

            const float seite = chip ? 1.0f : -1.0f;
            const float x_aussen_chip = m.x + seite * (breite * 0.5f);
            const float y_start = oben + (unten - oben) * 0.35f;
            const float x_rand = m.x + seite * R * 0.84f;
            const float y_ende = m.y - std::sqrt(std::fmax(0.0f, R * R - (x_rand - m.x) * (x_rand - m.x))) + D(3);
            const ImVec2 p[4] = {ImVec2(x_aussen_chip + seite * D(8), y_start),
                                 ImVec2(x_rand - seite * D(16), y_start),
                                 ImVec2(x_rand, y_start - D(16)), ImVec2(x_rand, y_ende)};
            dl->AddPolyline(p, 4, A(orange, 0.20f * a), 0, D(6.0f));
            dl->AddPolyline(p, 4, A(orange, 0.85f * a), 0, D(1.6f));
            if (aktiv) {
                float laenge = 0, seg[3];
                for (int i = 0; i < 3; ++i) {
                    seg[i] = std::sqrt((p[i + 1].x - p[i].x) * (p[i + 1].x - p[i].x) +
                                       (p[i + 1].y - p[i].y) * (p[i + 1].y - p[i].y));
                    laenge += seg[i];
                }
                for (int k = 0; k < 3; ++k) {
                    float s = std::fmod(t * D(55) + k * laenge / 3.0f + chip * 37.0f, laenge);
                    for (int i = 0; i < 3; ++i) {
                        if (s <= seg[i]) {
                            const float u = seg[i] > 0 ? s / seg[i] : 0;
                            const ImVec2 q(p[i].x + (p[i + 1].x - p[i].x) * u, p[i].y + (p[i + 1].y - p[i].y) * u);
                            dl->AddCircleFilled(q, D(6), A(orange, 0.25f * a), 16);
                            dl->AddCircleFilled(q, D(2.4f), IM_COL32(0xFF, 0xE0, 0xB0, (int)(255 * a)), 10);
                            break;
                        }
                        s -= seg[i];
                    }
                }
            }
        }
    }

    if (anzahl == 0) {
        const char* txt = T("Warte auf Speicherstellen ...");
        const ImVec2 g = ImGui::CalcTextSize(txt);
        dl->AddText(ImVec2(m.x - g.x * 0.5f, m.y + r * 0.30f), A(IM_COL32(0xE6, 0xE6, 0xE8, 0xFF), a), txt);
    }

    {
        const ImU32 weiss = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);

        for (int i = 0; i < 12; ++i) {
            const float u = i / 11.0f;
            dl->AddCircle(m, R * (0.86f + 0.14f * u), A(IM_COL32(0x00, 0x00, 0x00, 0xFF), 0.05f * u * a), 160,
                          R * 0.14f / 11.0f + 1.0f);
        }

        dl->AddCircleFilled(m, R, A(kHoch, 0.10f * a), 180);
        for (int i = 0; i < 20; ++i) {
            const float u = i / 19.0f;
            dl->AddCircle(m, R * (0.55f + 0.45f * u), A(kHoch, (0.01f + 0.05f * u * u * u) * a), 160,
                          R * 0.45f / 19.0f + 1.0f);
        }

        for (int i = 0; i < 90; ++i) {
            const float w0 = i * 6.2831853f / 90.0f, w1 = w0 + 6.2831853f / 90.0f;
            const float licht = 0.5f + 0.5f * std::cos(w0 - 3.93f);
            const ImU32 farbe = licht > 0.5f ? A(weiss, (licht - 0.5f) * 0.30f * a)
                                             : A(IM_COL32(0x60, 0x60, 0x5A, 0xFF), (0.5f - licht) * 0.30f * a);
            dl->PathClear();
            dl->PathArcTo(m, R - D(2), w0, w1, 2);
            dl->PathArcTo(m, r, w1, w0, 2);
            dl->PathFillConvex(farbe);
        }

        dl->AddCircle(m, R, IM_COL32(0x7C, 0x7B, 0x75, (int)(255 * a)), 180, D(1.6f));
        dl->AddCircle(m, R - D(2), A(weiss, 0.75f * a), 180, D(1.2f));
        dl->AddCircle(m, r, A(weiss, 0.55f * a), 160, D(1.2f));
        dl->AddCircle(m, r + D(1.5f), A(IM_COL32(0x50, 0x50, 0x4A, 0xFF), 0.25f * a), 160, 1.0f);
        for (int s = 0; s < 2; ++s) {
            const ImVec2 k = s ? form.knick_r : form.knick_l, rd = s ? form.rand_r : form.rand_l;
            dl->AddLine(k, rd, A(weiss, 0.55f * a), D(1.2f));
        }

        {
            const FrontMasse f = FrontMasseBerechnen(m, R);
            ImVec2 umriss[14];
            umriss[0] = ImVec2(m.x - f.trapez_oben, f.trapez_y);
            umriss[1] = ImVec2(m.x + f.trapez_oben, f.trapez_y);
            for (int i = 0; i <= 11; ++i) {
                const float x = m.x + f.breite - 2.0f * f.breite * i / 11.0f, dx = x - m.x;
                umriss[2 + i] = ImVec2(x, m.y + std::sqrt(R * R - dx * dx) - D(1));
            }
            dl->AddConvexPolyFilled(umriss, 14, A(IM_COL32(0xC8, 0xC9, 0xCD, 0xFF), 0.45f * a));
            dl->AddPolyline(umriss, 14, A(weiss, 0.70f * a), ImDrawFlags_Closed, D(1.1f));
        }

        auto streifen = [&](float abstand, float breite, float staerke, bool scharf) {
            const float nx = 0.7071f, ny = 0.7071f;
            const int n = 18;
            for (int i = 0; i < n; ++i) {
                const float u = i / (float)(n - 1);
                const float profil = scharf ? (1.0f - u) : std::sin(u * 3.14159f);
                const float d = abstand + u * breite;
                const float h = std::sqrt(std::fmax(0.0f, R * R - d * d)) - D(2);
                if (h <= 0) continue;
                const ImVec2 c(m.x + nx * d, m.y + ny * d);
                dl->AddLine(ImVec2(c.x - ny * h, c.y + nx * h), ImVec2(c.x + ny * h, c.y - nx * h),
                            A(weiss, staerke * profil * a), breite / n + 1.0f);
            }
        };
        const float wandern = std::sin(t * 0.25f) * R * 0.04f;
        streifen(-R * 0.62f + wandern, R * 0.30f, 0.16f, false);
        streifen(-R * 0.22f + wandern, R * 0.09f, 0.22f, true);
    }
    (void)RechteckImKreis;

}

void RamFenster() {
    FluegelUeberschrift("LAUFWERK");
    Abstand(2);
    if (g_vorschau && !g_sitzung.Laeuft()) theme::Marke("Vorschau", Zustand::Info);
    else theme::Marke(g_z.kabel_offen ? "wartet auf Spiel" : "Kabel nicht geöffnet", Zustand::Neutral);
    Abstand(4);
    ImGui::PushStyleColor(ImGuiCol_Text, F(IM_COL32(0x9A, 0xA0, 0xA8, 0xFF)));
    ImGui::PushFont(nullptr, theme::kGroesseKlein);
    ImGui::TextWrapped("%s", T("An der Dreamcast im DC-RAW-Menü ein Spiel mit A starten. Sobald es läuft, schließt "
                               "sich der Deckel und die Speicherstellen erscheinen auf der Spielseite."));
    ImGui::PopFont();
    ImGui::PopStyleColor();
    if (g_vorschau) {
        Abstand(4);
        if (theme::KnopfNeben("Deckel schließen")) g_vorschau = false;
    }
}

void Plakette(ImDrawList* dl, ImVec2 oben_mitte) {

    const float rk = D(19), bw = D(140), bh = D(24);
    const ImVec2 mk(oben_mitte.x, oben_mitte.y + rk);
    const ImU32 flaeche = IM_COL32(0xEC, 0xEE, 0xF1, 0xFF);
    const ImU32 rand = IM_COL32(0xB4, 0xB8, 0xBF, 0xFF);

    const ImVec2 sa(mk.x - bw * 0.5f, mk.y + rk * 0.35f), sb(mk.x + bw * 0.5f, mk.y + rk * 0.35f + bh);
    dl->AddCircle(mk, rk, rand, 40, 1.0f);
    dl->AddRect(sa, sb, rand, D(5), 0, 1.0f);
    dl->AddCircleFilled(mk, rk - 1.0f, flaeche, 40);
    dl->AddRectFilled(ImVec2(sa.x + 1.0f, sa.y + 1.0f), ImVec2(sb.x - 1.0f, sb.y - 1.0f), flaeche, D(5) - 1.0f);

    if (g_jason_plakette.ok()) {
        const float rr = rk * 0.84f;
        const ImVec2 mm(mk.x, mk.y - D(1));

        const float w = (float)g_jason_plakette.w, h = (float)g_jason_plakette.h;
        const ImVec2 p0(std::floor(mm.x - w * 0.5f), std::floor(mm.y - h * 0.5f));
        dl->AddImage((ImTextureID)(intptr_t)g_jason_plakette.id, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0),
                     ImVec2(g_jason_plakette.u, g_jason_plakette.v));
    } else {
        theme::Emblem(dl, ImVec2(mk.x, mk.y - D(2)), rk * 1.45f, (float)ImGui::GetTime());
    }

    ImFont* f = theme::gHalbfett ? theme::gHalbfett : ImGui::GetFont();
    const float g = D(15.0f);
    const ImVec2 t1 = f->CalcTextSizeA(g, FLT_MAX, 0.0f, "DREAM-");
    const ImVec2 t2 = f->CalcTextSizeA(g, FLT_MAX, 0.0f, "RAW");
    const float x = mk.x - (t1.x + t2.x) * 0.5f, y = sa.y + (bh - t1.y) * 0.5f;
    dl->AddText(f, g, ImVec2(x, y), IM_COL32(0x3A, 0x4A, 0x60, 0xFF), "DREAM-");
    dl->AddText(f, g, ImVec2(x + t1.x, y), theme::kAkzent, "RAW");
}

bool Anschluss(const char* id, char buchstabe, const char* name, Seite s, ImVec2 p, ImVec2 groesse) {
    name = T(name);
    ImGui::SetCursorScreenPos(p);
    const bool klick = ImGui::InvisibleButton(id, groesse);
    const bool ueber = ImGui::IsItemHovered();
    const bool aktiv = g_z.seite == s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + groesse.x, p.y + groesse.y);

    const ImU32 oeffnung = ueber || aktiv ? IM_COL32(0x4A, 0x4E, 0x55, 0xFF) : IM_COL32(0x3C, 0x40, 0x46, 0xFF);
    dl->AddRectFilled(p, q, oeffnung, D(10));
    dl->AddLine(ImVec2(p.x + D(10), p.y + 1.5f), ImVec2(q.x - D(10), p.y + 1.5f), IM_COL32(0x00, 0x00, 0x00, 0x60), 2.0f);
    dl->AddLine(ImVec2(p.x + D(10), q.y + 1.0f), ImVec2(q.x - D(10), q.y + 1.0f), IM_COL32(0xFF, 0xFF, 0xFF, 0xD0), 1.0f);
    if (aktiv) dl->AddRect(p, q, theme::kAkzent, D(10), 0, D(2.0f));
    const ImVec2 sa(p.x + groesse.x * 0.22f, q.y - D(11)), sb2(q.x - groesse.x * 0.22f, q.y - D(6));
    dl->AddRectFilled(sa, sb2, IM_COL32(0x6A, 0x6E, 0x75, 0xFF), D(2));

    ImFont* fh = theme::gHalbfett ? theme::gHalbfett : ImGui::GetFont();
    char b[2] = {buchstabe, 0};
    const float gb = D(15.0f), gn = D(15.0f);
    dl->AddText(fh, gb, ImVec2(p.x + D(10), p.y + D(6)), aktiv ? theme::kAkzentHell : IM_COL32(0xB8, 0xBC, 0xC2, 0xFF), b);
    const ImVec2 tn = fh->CalcTextSizeA(gn, FLT_MAX, 0.0f, name);
    dl->AddText(fh, gn, ImVec2(p.x + (groesse.x - tn.x) * 0.5f + D(6), p.y + D(7)),
                aktiv ? IM_COL32(0xFF, 0xFF, 0xFF, 0xFF) : IM_COL32(0xD8, 0xDA, 0xDE, 0xFF), name);
    if (klick) g_z.seite = s;
    return klick;
}

bool Rundknopf(const char* id, const char* text, Seite s, ImVec2 m, float r, bool rechts) {
    text = T(text);

    const float neigung = rechts ? -0.35f : 0.35f;
    const ImVec2 mulde(m.x + (rechts ? D(5) : -D(5)), m.y - D(4));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddEllipseFilled(mulde, ImVec2(r * 1.42f, r * 1.18f), IM_COL32(0xD7, 0xD6, 0xD0, 0xFF), neigung, 48);
    dl->AddEllipse(mulde, ImVec2(r * 1.42f, r * 1.18f), IM_COL32(0xA9, 0xA8, 0xA1, 0xFF), neigung, 48, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(m.x - r, m.y - r));
    const bool klick = ImGui::InvisibleButton(id, ImVec2(r * 2, r * 2));
    const bool ueber = ImGui::IsItemHovered();
    const bool aktiv = g_z.seite == s;
    dl->AddCircleFilled(ImVec2(m.x, m.y + D(2)), r, IM_COL32(0x00, 0x00, 0x00, 0x22), 48);
    dl->AddCircleFilled(m, r, ueber ? theme::kKnopfHover : theme::kKnopf, 48);
    dl->AddCircle(m, r, aktiv ? theme::kAkzent : IM_COL32(0xA9, 0xA8, 0xA1, 0xFF), 48, aktiv ? D(1.8f) : 1.0f);
    dl->PathArcTo(m, r - D(2), 3.14159f * 1.15f, 3.14159f * 1.85f, 24);
    dl->PathStroke(IM_COL32(0xFF, 0xFF, 0xFF, 0xC0), 0, D(1.5f));
    ImFont* f = theme::gHalbfett ? theme::gHalbfett : ImGui::GetFont();
    const float g = D(12.0f);
    const ImVec2 ts = f->CalcTextSizeA(g, FLT_MAX, 0.0f, text);
    dl->AddText(f, g, ImVec2(m.x - ts.x * 0.5f, m.y - ts.y * 0.5f), aktiv ? theme::kAkzent : theme::kLeise, text);
    if (klick) g_z.seite = s;
    return klick;
}

void FluegelKonto() {
    FluegelUeberschrift("RETROACHIEVEMENTS");
    Abstand(2);
    if (g_z.angemeldet) {
        theme::Marke("angemeldet", Zustand::Gut);
        ImGui::PushFont(theme::gHalbfett, theme::kGroesseZwisch);
        ImGui::TextWrapped("%s", g_z.benutzer.c_str());
        ImGui::PopFont();
        Leise("%s", Fmt(T("%1 Punkte"), {std::to_string(g_z.punkte)}).c_str());
    } else {
        theme::Marke("nicht angemeldet", Zustand::Neutral);
        Abstand(2);
        if (theme::KnopfNeben("Anmelden")) g_z.seite = Seite::Konto;
    }
    Abstand(10);
    FluegelUeberschrift("SPIEL");
    Abstand(2);
    if (g_z.spiel.ok) {
        ImGui::PushFont(theme::gHalbfett, 0.0f);
        ImGui::TextWrapped("%s", g_z.spiel.titel.c_str());
        ImGui::PopFont();

        SpielIconAnfordern(g_z.spiel.id, g_z.spiel.icon_url);
        if (g_spiel_icon.ok() && g_spiel_icon_id == g_z.spiel.id) {
            Abstand(2);
            ImGui::Image((ImTextureID)(intptr_t)g_spiel_icon.id, ImVec2((float)g_spiel_icon.w, (float)g_spiel_icon.h),
                         ImVec2(0, 0), ImVec2(g_spiel_icon.u, g_spiel_icon.v));
        }
    } else if (!g_z.dc_titel.empty() && !g_z.im_menue) {

        ImGui::PushFont(theme::gHalbfett, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kGut);
        ImGui::TextWrapped("%s", g_z.dc_titel.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        theme::Hinweis(g_z.dc_unbekannt ? "Dieses Abbild kennt RetroAchievements nicht." : "wird bei RA nachgeschlagen ...");
    } else {
        theme::Hinweis(g_z.spiel_id ? "erkannt, Liste nach Anmeldung" : "noch keins erkannt");
    }
}

bool g_fach_offen = false;
float g_fach = 0.0f;

void AchievementFach(ImVec2 a, ImVec2 b, float oben, float unten) {
    const float ziel = g_fach_offen ? 1.0f : 0.0f;
    g_fach += (ziel - g_fach) * std::min(1.0f, ImGui::GetIO().DeltaTime * 12.0f);
    if (std::fabs(ziel - g_fach) < 0.003f) g_fach = ziel;
    if (g_fach <= 0.0f) return;
    const float w = std::min(D(420), (b.x - a.x) * 0.48f);
    const float x0 = b.x - w * g_fach;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0 - D(10), oben + D(4)), ImVec2(b.x, unten + D(6)), IM_COL32(0, 0, 0, 0x22), D(12));
    ImGui::SetCursorScreenPos(ImVec2(x0, oben));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, F(theme::kKarte));
    ImGui::PushStyleColor(ImGuiCol_Border, F(theme::kKartenrand));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(D(18), D(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, D(12));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::BeginChild("##fach", ImVec2(w, unten - oben), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    FluegelUeberschrift("ACHIEVEMENTS");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
    if (ImGui::Button("×", ImVec2(ImGui::GetFrameHeight(), 0))) g_fach_offen = false;
    Abstand(4);
    SpielInhalt(0.0f);
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void FluegelVerbindung() {
    FluegelUeberschrift("VERBINDUNG");
    Abstand(2);
    if (!g_sitzung.Laeuft()) {
        theme::Marke("Kabel nicht verbunden", Zustand::Neutral);
        Abstand(2);
        if (theme::KnopfNeben("Verbinden")) g_z.seite = Seite::Netz;
    } else {
        theme::Marke(g_z.kabel_offen ? "Kabel geöffnet" : "Kabel nicht geöffnet",
                     g_z.kabel_offen ? Zustand::Gut : Zustand::Warnung);
        Abstand(2);
        theme::Marke(g_z.im_menue ? "DC-RAW-Menü" : g_z.daten_kommen ? "Spiel läuft" : g_z.daten_aktuell ? "Dreamcast sendet" : "wartet auf Dreamcast",
                     (g_z.im_menue || g_z.daten_aktuell || g_z.daten_kommen) ? Zustand::Gut : Zustand::Neutral);
    }
    Abstand(6);

    if (theme::KnopfNeben(g_fach_offen ? "Achievements ×" : "Achievements ›")) g_fach_offen = !g_fach_offen;
    Abstand(10);
    FluegelUeberschrift("LETZTE MELDUNG");
    Abstand(2);
    if (!g_z.log.empty()) {
        const Logzeile& z = g_z.log.back();
        ImGui::PushStyleColor(ImGuiCol_Text, F(z.art == Zustand::Fehler ? theme::kFehler
                                               : z.art == Zustand::Warnung ? theme::kWarnung
                                               : z.art == Zustand::Gut ? theme::kGut : theme::kLeise));
        ImGui::PushFont(nullptr, theme::kGroesseKlein);
        ImGui::TextWrapped("%s", z.text.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
    }
    if (g_z.laeuft > 0) {
        Abstand(6);
        theme::Drehkreis(D(7));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, F(theme::kAkzent));
        ImGui::TextUnformatted(T("arbeitet ..."));
        ImGui::PopStyleColor();
    }
}

void Zeichnen() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##haupt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    DeckelBewegen();

    const ImVec2 a0 = vp->WorkPos;
    const ImVec2 b0(a0.x + vp->WorkSize.x, a0.y + vp->WorkSize.y);
    const float ins = D(10), rnd = D(30);
    const ImVec2 a(a0.x + ins, a0.y + ins);
    const ImVec2 b(b0.x - ins, b0.y - ins);
    const float w = b.x - a.x;
    const float blende = D(86);
    const float unten = b.y - blende;
    const float sb = D(12);
    const float fb = D(14);
    const float oberseite = unten - fb;
    const float hoehe = oberseite - a.y;

    float R = hoehe * 0.455f;
    if (R > w * 0.29f) R = w * 0.29f;
    const float r = R * 0.77f;
    const ImVec2 m(a.x + w * 0.5f, a.y + hoehe * 0.50f);

    const float pi = 3.14159265f;
    const ImU32 kLicht = IM_COL32(0xFF, 0xFF, 0xFF, 0xB0);

    dl->AddRectFilled(a0, b0, IM_COL32(0xC9, 0xC8, 0xC3, 0xFF));
    dl->AddRectFilled(ImVec2(a.x, a.y + D(5)), ImVec2(b.x, b.y + D(5)), IM_COL32(0x00, 0x00, 0x00, 0x26), rnd);
    dl->AddRectFilled(a, b, theme::kGrund, rnd);

    {
        const ImU32 kSchraege = IM_COL32(0xD6, 0xD5, 0xCF, 0xFF);
        const ImVec2 l[4] = {ImVec2(a.x, a.y + rnd), ImVec2(a.x + sb, a.y + rnd + sb), ImVec2(a.x + sb, oberseite),
                             ImVec2(a.x, unten)};
        dl->AddConvexPolyFilled(l, 4, kSchraege);
        dl->AddLine(l[1], l[2], kLicht, 1.2f * g_dpi);
        const ImVec2 rr[4] = {ImVec2(b.x, a.y + rnd), ImVec2(b.x, unten), ImVec2(b.x - sb, oberseite),
                              ImVec2(b.x - sb, a.y + rnd + sb)};
        dl->AddConvexPolyFilled(rr, 4, kSchraege);
        dl->AddLine(rr[3], rr[2], IM_COL32(0xB8, 0xB7, 0xB0, 0xC0), 1.0f);
        const ImVec2 v[4] = {ImVec2(a.x + sb, oberseite), ImVec2(b.x - sb, oberseite), ImVec2(b.x, unten),
                             ImVec2(a.x, unten)};
        dl->AddConvexPolyFilled(v, 4, IM_COL32(0xD1, 0xD0, 0xC9, 0xFF));
        dl->AddLine(v[0], v[1], kLicht, 1.2f * g_dpi);
    }

    const float wl = pi * 1.146f, wr = pi * (2.0f - 0.146f);
    const ImVec2 knick_l(m.x + std::cos(wl) * r, m.y + std::sin(wl) * r);
    const ImVec2 knick_r(m.x + std::cos(wr) * r, m.y + std::sin(wr) * r);

    const float steig = 70.0f / 154.0f;
    const ImVec2 oben_l(knick_l.x - (knick_l.y - a.y) * steig, a.y);
    const ImVec2 oben_r(knick_r.x + (knick_r.y - a.y) * steig, a.y);

    auto schnitt = [&](ImVec2 aussen, ImVec2 innen) {
        float lo = 0.0f, hi = 1.0f;
        for (int k = 0; k < 30; ++k) {
            const float t = (lo + hi) * 0.5f;
            const float x = aussen.x + (innen.x - aussen.x) * t - m.x;
            const float y = aussen.y + (innen.y - aussen.y) * t - m.y;
            if (x * x + y * y > R * R) lo = t; else hi = t;
        }
        return ImVec2(aussen.x + (innen.x - aussen.x) * lo, aussen.y + (innen.y - aussen.y) * lo);
    };
    const ImVec2 rand_l = schnitt(oben_l, knick_l), rand_r = schnitt(oben_r, knick_r);

    {
        const ImDrawListFlags alt = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        dl->AddRectFilled(ImVec2(a.x + sb, a.y), ImVec2(b.x - sb, oberseite), IM_COL32(0x80, 0x7F, 0x78, 0x10), rnd,
                          ImDrawFlags_RoundCornersTop);

        float ul = std::atan2(rand_l.y - m.y, rand_l.x - m.x);
        float ur = std::atan2(rand_r.y - m.y, rand_r.x - m.x);
        if (ul < 0) ul += 2.0f * pi;
        if (ur < 0) ur += 2.0f * pi;
        const int n = 80;
        for (int i = 0; i < n; ++i) {
            const float g0 = (float)i / n, g1 = (float)(i + 1) / n;
            auto kante = [&](float g) {
                return ImVec2(oben_l.x + (oben_r.x - oben_l.x) * g, a.y);
            };
            auto bogen_pkt = [&](float g) {
                const float t = ul + (ur - ul) * g;
                return ImVec2(m.x + std::cos(t) * R, m.y + std::sin(t) * R);
            };
            dl->AddQuadFilled(kante(g0), kante(g1), bogen_pkt(g1), bogen_pkt(g0), kHoch);
        }
        dl->Flags = alt;
    }

    auto aussen = [](ImVec2 p, ImVec2 q, float seite) {
        const float dx = q.x - p.x, dy = q.y - p.y, len = std::sqrt(dx * dx + dy * dy);
        ImVec2 n(-dy / len, dx / len);
        if (n.x * seite < 0) n = ImVec2(-n.x, -n.y);
        return n;
    };
    const ImVec2 n_l = aussen(oben_l, knick_l, -1.0f), n_r = aussen(oben_r, knick_r, 1.0f);
    auto gleich = [](ImVec2 p) { return p; };

    SchattenKnick(dl, oben_l, rand_l, n_l, gleich);
    SchattenKnick(dl, oben_r, rand_r, n_r, gleich);

    const float winkel = g_deckel * 1.30f;
    const float s = std::cos(winkel);
    const float scharnier = m.y - R;

    const float hub = std::sin(winkel);
    auto T = [&](ImVec2 p) {
        const float u = (p.y - scharnier) / (2.0f * R);
        const float f = 1.0f + 0.28f * hub * u;
        return ImVec2(m.x + (p.x - m.x) * f, scharnier + (p.y - scharnier) * s);
    };

    if (g_deckel > 0.001f) {
        dl->AddCircleFilled(m, R, IM_COL32(0x2C, 0x30, 0x36, 0xFF), 180);
        dl->AddCircle(m, R * 0.80f, IM_COL32(0x3A, 0x3F, 0x46, 0xFF), 160, D(1.5f));
        dl->AddCircleFilled(m, R * 0.07f, IM_COL32(0x8E, 0x93, 0x9A, 0xFF), 32);
        dl->AddCircleFilled(m, R * 0.03f, IM_COL32(0x2C, 0x30, 0x36, 0xFF), 24);

        if ((SeqLaeuft() && SeqZeit() >= kFlug) || (!SeqLaeuft() && DcVerbunden()))
            ScheibeZeichnen(dl, m, r * 0.92f, (float)ImGui::GetTime() * 0.9f, 1.0f);

        for (int i = 0; i < 10; ++i) {
            const float y = scharnier + R * 0.12f * i;
            const float h = std::sqrt(std::fmax(0.0f, R * R - (y - m.y) * (y - m.y)));
            dl->AddRectFilled(ImVec2(m.x - h, y), ImVec2(m.x + h, y + R * 0.12f),
                              IM_COL32(0x00, 0x00, 0x00, (int)(110 * hub * (1.0f - i / 10.0f))));
        }
    }

    {

        const int n = 160;
        ImVec2 pts[n];
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / n * 2.0f * pi;
            pts[i] = T(ImVec2(m.x + std::cos(t) * R, m.y + std::sin(t) * R));
        }

        if (hub > 0.01f) {
            const float dicke = D(18) * hub;
            ImVec2 kante[n];
            for (int i = 0; i < n; ++i) kante[i] = ImVec2(pts[i].x, pts[i].y + dicke);
            dl->AddConvexPolyFilled(kante, n, IM_COL32(0xB4, 0xB3, 0xAD, 0xFF));
            dl->AddPolyline(kante, n, IM_COL32(0x7C, 0x7B, 0x75, 0xFF), ImDrawFlags_Closed, 1.0f);
        }

        const ImU32 deckel = kHoch;
        dl->AddConvexPolyFilled(pts, n, deckel);

        auto bogen = [&](float radius, ImU32 farbe, float dicke) {
            dl->PathClear();
            for (int i = 0; i <= 120; ++i) {
                const float t = wl + ((wr - 2.0f * pi) - wl) * i / 120.0f;
                dl->PathLineTo(T(ImVec2(m.x + std::cos(t) * radius, m.y + std::sin(t) * radius)));
            }
            dl->PathStroke(farbe, 0, dicke);
        };

        {
            const float ul = std::atan2(rand_l.y - m.y, rand_l.x - m.x);
            float ur = std::atan2(rand_r.y - m.y, rand_r.x - m.x);
            float ul2 = ul < 0 ? ul + 2.0f * pi : ul;
            if (ur > 0) ur -= 2.0f * pi;
            const int nw = 120, nr = 8;

            const ImDrawListFlags alt = dl->Flags;
            dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            for (int k = 0; k < nr; ++k) {
                const float f0 = (float)k / nr, f1 = (float)(k + 1) / nr;
                const int alpha = (int)(0x14 - 0x0E * f0);
                const ImU32 farbe = IM_COL32(0x80, 0x7F, 0x78, alpha);
                for (int i = 0; i < nw; ++i) {
                    const float g0 = (float)i / nw, g1 = (float)(i + 1) / nw;
                    auto pkt = [&](float g, float f) {
                        const float ti = wl + ((wr - 2.0f * pi) - wl) * g;
                        const float to = ul2 + (ur - ul2) * g;
                        const ImVec2 innen(m.x + std::cos(ti) * r, m.y + std::sin(ti) * r);
                        const ImVec2 aussen2(m.x + std::cos(to) * R, m.y + std::sin(to) * R);
                        return T(ImVec2(innen.x + (aussen2.x - innen.x) * f, innen.y + (aussen2.y - innen.y) * f));
                    };
                    dl->AddQuadFilled(pkt(g0, f0), pkt(g1, f0), pkt(g1, f1), pkt(g0, f1), farbe);
                }
            }
            dl->Flags = alt;
        }
        bogen(r - 1.0f * g_dpi, kRilleLicht, 1.0f * g_dpi);

        SchattenKnick(dl, rand_l, knick_l, n_l, T);
        SchattenKnick(dl, rand_r, knick_r, n_r, T);

        {
            const FrontMasse f = FrontMasseBerechnen(m, R);
            auto unten = [&](float x) { const float dx = x - m.x; return ImVec2(x, m.y + std::sqrt(R * R - dx * dx) - D(1)); };
            ImVec2 pkt[14];
            pkt[0] = T(ImVec2(m.x - f.trapez_oben, f.trapez_y));
            pkt[1] = T(ImVec2(m.x + f.trapez_oben, f.trapez_y));
            for (int i = 0; i <= 11; ++i) pkt[2 + i] = T(unten(m.x + f.breite - 2.0f * f.breite * i / 11.0f));
            dl->AddConvexPolyFilled(pkt, 14, IM_COL32(0xCF, 0xD0, 0xD3, 0xFF));

            ImVec2 unterteil[14];
            for (int i = 0; i < 14; ++i)
                unterteil[i] = i < 2 ? ImVec2(pkt[i].x + (pkt[i == 0 ? 13 : 2].x - pkt[i].x) * 0.5f,
                                              pkt[i].y + (pkt[i == 0 ? 13 : 2].y - pkt[i].y) * 0.5f)
                                     : pkt[i];
            dl->AddConvexPolyFilled(unterteil, 14, IM_COL32(0xB8, 0xB9, 0xBE, 0x60));
            dl->AddLine(pkt[0], pkt[1], IM_COL32(0xFF, 0xFF, 0xFF, 0xC0), 1.2f * g_dpi);
            dl->AddLine(pkt[0], pkt[13], kRille, 1.0f * g_dpi);
            dl->AddLine(pkt[1], pkt[2], kRille, 1.0f * g_dpi);
        }
    }

    dl->AddCircle(m, R, IM_COL32(0x7C, 0x7B, 0x75, 0xFF), 180, 1.6f * g_dpi);
    dl->AddCircle(ImVec2(m.x, m.y + 1.5f * g_dpi), R + 1.0f * g_dpi, kRilleLicht, 180, 1.0f * g_dpi);

    {
        const FrontMasse f = FrontMasseBerechnen(m, R);
        const float oben = m.y + std::sqrt(R * R - f.breite * f.breite) + 1.5f * g_dpi;
        const ImVec2 s1(m.x - f.breite, oben);
        const ImVec2 s2(m.x + f.breite, oben);
        const ImVec2 sp(m.x, f.leuchte_spitze);
        (void)oberseite;

        const bool glimmt = DcVerbunden() || g_vorschau;
        if (glimmt) {
            const float puls = 0.80f + 0.20f * std::sin((float)ImGui::GetTime() * 2.4f);
            const ImVec2 mitte((s1.x + s2.x + sp.x) / 3.0f, (s1.y + s2.y + sp.y) / 3.0f);
            const float hof = (s2.x - s1.x) * 1.1f;
            for (int i = 4; i >= 1; --i)
                dl->AddCircleFilled(mitte, hof * i / 4.0f, IM_COL32(0xFF, 0x8A, 0x1E, (int)(0x16 * puls)), 32);
            dl->AddTriangleFilled(s1, s2, sp, IM_COL32(0xF5, 0x7E, 0x14, (int)(0xFF * puls)));
            dl->AddTriangleFilled(s1, ImVec2((s1.x + s2.x) * 0.5f, s1.y), sp,
                                  IM_COL32(0xFF, 0xB0, 0x5A, (int)(0xC0 * puls)));
        } else {

            dl->AddTriangleFilled(s1, s2, sp, IM_COL32(0x2E, 0x31, 0x37, 0xFF));
            dl->AddTriangleFilled(s1, ImVec2((s1.x + s2.x) * 0.5f, s1.y), sp, IM_COL32(0x45, 0x48, 0x4F, 0xFF));
        }

        dl->AddLine(ImVec2(s1.x + (sp.x - s1.x) * 0.25f, s1.y + (sp.y - s1.y) * 0.25f + 1.0f),
                    ImVec2(s1.x + (sp.x - s1.x) * 0.55f, s1.y + (sp.y - s1.y) * 0.55f + 1.0f),
                    IM_COL32(0xFF, 0xFF, 0xFF, 0x90), 1.0f * g_dpi);
        dl->AddLine(s1, sp, kRille, 1.0f * g_dpi);
        dl->AddLine(s2, sp, kRille, 1.0f * g_dpi);
    }

    const Leuchte ra[5] = {
        {"Konto angemeldet", g_z.angemeldet},
        {"Spiel erkannt", g_z.spiel_id != 0},
        {"Achievement-Liste geladen", g_z.spiel.ok},
        {"Freischaltungen aktiv", DcVerbunden() && g_z.spiel.ok},
        {"Hardcore", false},
    };
    const Leuchte netz[5] = {
        {"Sitzung gestartet", g_sitzung.Laeuft()},
        {"Kabel geöffnet", g_z.kabel_offen},
        {"Dreamcast sendet", g_z.daten_aktuell},
        {"RAM-Werte pro Bild", g_z.bilder_pro_s > 0},
        {"Spiel läuft", DcVerbunden() && g_z.spiel_id != 0},
    };
    Leuchten(dl, m, R * 1.07f, true, ra, 5);
    Leuchten(dl, m, R * 1.07f, false, netz, 5);

    {
        if (SeqLaeuft()) {

            g_roentgen = Glatt((SeqZeit() - kFlug - kZu) / kRoe);
        } else {
            const float ziel = RoentgenAktiv() ? 1.0f : 0.0f;
            g_roentgen += (ziel - g_roentgen) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 5.0f));
            if (std::fabs(ziel - g_roentgen) < 0.002f) g_roentgen = ziel;
        }
    }
    {
        HuellenForm form;
        form.knick_l = knick_l; form.knick_r = knick_r;
        form.rand_l = rand_l;   form.rand_r = rand_r;
        form.vorderkante = oberseite;
        RoentgenBlick(dl, m, R, r, form);
    }
    FlugScheibe(dl, m, R, r * 0.92f);

    if (g_deckel < 0.02f && (g_roentgen < 0.5f || CdTitel()[0] == 0)) {
        const ImVec2 po(m.x, m.y - r + D(12));
        Plakette(dl, po);

        ImGui::SetCursorScreenPos(ImVec2(po.x - D(54), po.y));
        if (ImGui::InvisibleButton("##plakette", ImVec2(D(108), D(52)))) g_katze_oeffnen = true;
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    const float rk = D(32.5f);
    const float ky = oberseite - rk - D(22);
    const ImVec2 knopf_l(a.x + sb + rk + D(30), ky), knopf_r(b.x - sb - rk - D(30), ky);
    Rundknopf("##setup", "SETUP", Seite::Setup, knopf_l, rk, false);
    Rundknopf("##info", "OPTIONEN", Seite::Info, knopf_r, rk, true);

    const float rand = sb + D(16);
    const float fl_breite = (m.x - R * 1.15f) - (a.x + rand) - D(6);
    const float fl_oben = a.y + hoehe * 0.30f;
    const float fl_hoehe = (ky - rk * 1.4f - D(10)) - fl_oben;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::SetCursorScreenPos(ImVec2(a.x + rand, fl_oben));
    ImGui::BeginChild("##links", ImVec2(fl_breite, fl_hoehe), 0, ImGuiWindowFlags_NoScrollbar);
    FluegelKonto();
    ImGui::EndChild();
    ImGui::SetCursorScreenPos(ImVec2(m.x + R * 1.15f + D(6), fl_oben));
    ImGui::BeginChild("##rechts", ImVec2(fl_breite, fl_hoehe), 0, ImGuiWindowFlags_NoScrollbar);
    FluegelVerbindung();
    ImGui::EndChild();

    if (g_deckel < 0.02f) {

        const float h2 = r * 0.60f;
        const float w2 = std::sqrt(r * r - h2 * h2) - D(4);
        float iw = w2 * 2.0f - D(24);
        float io = m.y - h2 + D(10);
        float ih = h2 * 2.0f - D(20);
        if (g_roentgen > 0.5f && g_z.seite == Seite::Spiel) {

            iw = r * 1.62f;
            io = m.y + r * 0.64f;
            ih = r * 0.30f;
        } else {
            const ImVec2 fa(m.x - w2, m.y - h2), fb(m.x + w2, m.y + h2);

            dl->AddRectFilled(fa, fb, IM_COL32(0xFF, 0xFF, 0xFF, 0x22), D(10));
            dl->AddRect(fa, fb, IM_COL32(0x80, 0x7F, 0x78, 0x30), D(10), 0, D(1.0f));
        }
        ImGui::SetCursorScreenPos(ImVec2(m.x - iw * 0.5f, io));
        theme::KreisSetzen(m, r - D(8));
        ImGui::BeginChild("##deckel", ImVec2(iw, ih), 0);

        if (g_z.seite == Seite::Spiel && g_z.daten_kommen && !g_z.dc_titel.empty() && !g_z.animation_gespielt && g_seq_start < 0) {
            g_z.animation_gespielt = true;
            g_seq_vorschau = false;
            SeqStarten();
        }
        switch (g_z.seite) {
            case Seite::Spiel: SeiteSpiel(); break;
            case Seite::Konto: SeiteKonto(); break;
            case Seite::Netz:  SeiteNetz(); break;
            case Seite::Log:   SeiteLog(); break;
            case Seite::Setup: SeiteSetup(); break;
            case Seite::Info:  SeiteInfo(); break;
        }
        ImGui::EndChild();
        theme::KreisSetzen(m, 0.0f);
    } else if (g_deckel > 0.6f && !SeqLaeuft()) {

        const float iw = R * 1.30f;
        const float io = scharnier + 2.0f * R * s + D(14);
        const float ih = (m.y + R * 0.72f) - io;
        if (ih > D(60)) {
            ImGui::SetCursorScreenPos(ImVec2(m.x - iw * 0.5f, io));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, (g_deckel - 0.6f) / 0.4f);
            ImGui::BeginChild("##ram", ImVec2(iw, ih), 0);
            RamFenster();
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
    }
    ImGui::PopStyleVar();

    dl->AddRectFilled(ImVec2(a.x, unten), b, theme::kLeiste, rnd, ImDrawFlags_RoundCornersBottom);
    dl->AddLine(ImVec2(a.x, unten), ImVec2(b.x, unten), theme::kKartenrand, 1.0f);

    const ImVec2 port(D(112), D(46));
    const float abstand = D(16);
    const float gesamt = port.x * 4 + abstand * 3;
    const float px = m.x - gesamt * 0.5f, py = unten + (blende - port.y) * 0.5f;
    {
        const ImVec2 ra2(px - D(14), py - D(9)), rb2(px + gesamt + D(14), py + port.y + D(9));
        dl->AddRectFilled(ra2, rb2, IM_COL32(0xCC, 0xCB, 0xC4, 0xFF), D(12));
        dl->AddLine(ImVec2(ra2.x + D(12), ra2.y + 1.5f), ImVec2(rb2.x - D(12), ra2.y + 1.5f),
                    IM_COL32(0x00, 0x00, 0x00, 0x22), 2.0f);
        dl->AddLine(ImVec2(ra2.x + D(12), rb2.y + 1.0f), ImVec2(rb2.x - D(12), rb2.y + 1.0f), kLicht, 1.0f);
    }
    Anschluss("##A", 'A', "Spiel", Seite::Spiel, ImVec2(px, py), port);
    Anschluss("##B", 'B', "Konto", Seite::Konto, ImVec2(px + (port.x + abstand), py), port);
    Anschluss("##C", 'C', "Netz", Seite::Netz, ImVec2(px + (port.x + abstand) * 2, py), port);
    Anschluss("##D", 'D', "Log", Seite::Log, ImVec2(px + (port.x + abstand) * 3, py), port);

    {
        const char* hinweis = "DREAM-RAW \xE2\x80\x94 \xC2\xA9 2026 Liqui";
        ImGui::PushFont(theme::gNormal, theme::kGroesseKlein);
        const ImVec2 ts = ImGui::CalcTextSize(hinweis);
        const float hx = a.x + D(34);
        if (hx + ts.x < px - D(24))
            dl->AddText(ImVec2(hx, unten + (blende - ts.y) * 0.5f), IM_COL32(0x88, 0x88, 0x88, 0xFF), hinweis);
        ImGui::PopFont();
    }
    AchievementFach(a, b, a.y + D(14), unten - D(14));
    KatzeFenster();
    ErstEinrichtungFenster();
    BereitFenster();
    UpdateFenster();
    NachrichtFenster();

    ImGui::End();
}

bool GlAnlegen(HWND hwnd) {
    g_dc = ::GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int pf = ::ChoosePixelFormat(g_dc, &pfd);
    if (pf == 0 || !::SetPixelFormat(g_dc, pf, &pfd)) return false;
    g_rc = ::wglCreateContext(g_dc);
    return g_rc && ::wglMakeCurrent(g_dc, g_rc);
}

void GlFreigeben(HWND hwnd) {
    ::wglMakeCurrent(nullptr, nullptr);
    if (g_rc) { ::wglDeleteContext(g_rc); g_rc = nullptr; }
    if (g_dc) { ::ReleaseDC(hwnd, g_dc); g_dc = nullptr; }
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) ::glViewport(0, 0, LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = (LONG)(1000 * g_dpi);
            mm->ptMinTrackSize.y = (LONG)(760 * g_dpi);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    i18n::SpracheSetzen(i18n::SystemSprache());

    if (const wchar_t* a = _wgetenv(L"APPDATA")) {
        const std::wstring alt = std::wstring(a) + L"\\DC-RAW", neu = std::wstring(a) + L"\\DREAM-RAW";
        if (::GetFileAttributesW(neu.c_str()) == INVALID_FILE_ATTRIBUTES &&
            ::GetFileAttributesW(alt.c_str()) != INVALID_FILE_ATTRIBUTES)
            ::MoveFileW(alt.c_str(), neu.c_str());
    }
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = ::CreateSolidBrush(RGB(0xE6, 0xE5, 0xE0));
    wc.hIcon = (HICON)::LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)::LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"DreamRaw";
    ::RegisterClassExW(&wc);

    wchar_t titel[64];
    std::swprintf(titel, 64, L"DREAM-RAW  (Build %d)", buildinfo::kBuild);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, titel, WS_OVERLAPPEDWINDOW,
                                100, 40, 1000, 960, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    g_fenster = hwnd;

    g_dpi = (float)::GetDpiForWindow(hwnd) / 96.0f;
    if (g_dpi < 0.5f || g_dpi > 4.0f) g_dpi = 1.0f;
    {

        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        int b = (int)(1000 * g_dpi), h = (int)(960 * g_dpi);
        if (::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            const RECT& a = mi.rcWork;
            if (b > a.right - a.left) b = a.right - a.left;
            if (h > a.bottom - a.top) h = a.bottom - a.top;
            ::SetWindowPos(hwnd, nullptr, a.left + ((a.right - a.left) - b) / 2,
                           a.top + ((a.bottom - a.top) - h) / 2, b, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    if (!GlAnlegen(hwnd)) {
        ::MessageBoxW(nullptr, core::NachWide(T("OpenGL-Kontext konnte nicht angelegt werden.")).c_str(), L"DREAM-RAW", MB_ICONERROR);
        ::DestroyWindow(hwnd);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    theme::Apply(g_dpi);
    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL2_Init();

    bilder::Start();
    {
        const void* d = nullptr;
        size_t n = 0;
        if (bilder::Ressource(kJasonRessource, &d, &n)) {
            g_jason = bilder::PngHochladenSkaliert(d, n, static_cast<int>(std::lround(D(53))));
            g_jason_gross = bilder::PngHochladenSkaliert(d, n, static_cast<int>(std::lround(D(212))));

            g_jason_plakette = bilder::PngHochladenSkaliert(d, n, static_cast<int>(std::lround(D(26))));
        }
    }

    g_einstellungs_pfad = core::EinstellungsPfad();
    i18n::SpracheSetzen(i18n::SystemSprache());
    Log(std::string("DREAM-RAW ") + buildinfo::string());

    g_erst_oeffnen = g_einstellungs_pfad.empty() ||
                     ::GetFileAttributesW(g_einstellungs_pfad.c_str()) == INVALID_FILE_ATTRIBUTES ||
                     std::wstring(::GetCommandLineW()).find(L"--ersteinrichtung") != std::wstring::npos;
    EinstellungenLaden();
    AutomatischAnmelden();
    UpdatePruefen();

    bool laeuft = true;
    while (laeuft) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) laeuft = false;
        }
        if (!laeuft) break;
        if (::IsIconic(hwnd)) { ::Sleep(50); continue; }

        AuftraegeAusfuehren();

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Zeichnen();
        ImGui::Render();
        const ImVec4 g = F(theme::kGrund);
        ::glClearColor(g.x, g.y, g.z, 1.0f);
        ::glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        ::SwapBuffers(g_dc);
    }

    SecureZeroMemory(g_z.ra_passwort, sizeof(g_z.ra_passwort));
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    GlFreigeben(hwnd);
    ::DestroyWindow(hwnd);

    popup::Beenden();
    ::ExitProcess(0);
}
