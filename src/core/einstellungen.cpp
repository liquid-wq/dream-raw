// Settings.

#include "core/einstellungen.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "core/text.h"

namespace core {
namespace {

std::string Einzeilig(const std::string& s) {
    std::string aus;
    for (char c : s)
        if (c != '\r' && c != '\n') aus.push_back(c);
    return aus;
}

}

std::wstring EinstellungsPfad() {
    wchar_t basis[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", basis, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring ordner = std::wstring(basis) + L"\\DREAM-RAW";
    ::CreateDirectoryW(ordner.c_str(), nullptr);
    return ordner + L"\\einstellungen.txt";
}

bool EinstellungenLaden(const std::wstring& pfad, Einstellungen& e) {
    FILE* f = ::_wfopen(pfad.c_str(), L"rb");
    if (!f) return false;
    char zeile[2048];
    while (std::fgets(zeile, sizeof(zeile), f)) {
        std::string z = zeile;
        while (!z.empty() && (z.back() == '\n' || z.back() == '\r')) z.pop_back();
        const size_t gl = z.find('=');
        if (gl == std::string::npos) continue;
        const std::string k = z.substr(0, gl);
        const std::string v = z.substr(gl + 1);
        if (k == "ra_benutzer")          e.ra_benutzer = v;
        else if (k == "dreampi_host")    e.dreampi_host = v;
        else if (k == "dreamshell_host") e.dreamshell_host = v;
        else if (k == "dreamshell_port") { const int p = PortLesen(v); e.dreamshell_port = p > 0 ? p : 0; }
        else if (k == "angemeldet_bleiben") e.angemeldet_bleiben = (v == "1");
        else if (k == "ra_token")        e.ra_token = v;
        else if (k == "kabel_port")      e.kabel_port = v;
        else if (k == "nachricht_gelesen") e.nachricht_gelesen = std::atoll(v.c_str());
        else if (k == "sprache")         e.sprache = v;
    }
    std::fclose(f);
    return true;
}

bool EinstellungenSpeichern(const std::wstring& pfad, const Einstellungen& e) {

    const std::wstring tmp = pfad + L".neu";
    FILE* f = ::_wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    std::fprintf(f, "ra_benutzer=%s\n", Einzeilig(e.ra_benutzer).c_str());
    std::fprintf(f, "dreampi_host=%s\n", Einzeilig(e.dreampi_host).c_str());
    std::fprintf(f, "dreamshell_host=%s\n", Einzeilig(e.dreamshell_host).c_str());
    std::fprintf(f, "dreamshell_port=%d\n", e.dreamshell_port);
    std::fprintf(f, "angemeldet_bleiben=%d\n", e.angemeldet_bleiben ? 1 : 0);
    std::fprintf(f, "ra_token=%s\n", e.angemeldet_bleiben ? Einzeilig(e.ra_token).c_str() : "");
    std::fprintf(f, "kabel_port=%s\n", Einzeilig(e.kabel_port).c_str());
    std::fprintf(f, "nachricht_gelesen=%lld\n", e.nachricht_gelesen);
    std::fprintf(f, "sprache=%s\n", Einzeilig(e.sprache).c_str());
    const bool ok = std::fflush(f) == 0;
    std::fclose(f);
    if (!ok) { ::DeleteFileW(tmp.c_str()); return false; }
    return ::MoveFileExW(tmp.c_str(), pfad.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

}
