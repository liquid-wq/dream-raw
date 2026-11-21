// HTTP requests to RetroAchievements.

#include "core/http.h"

#include "i18n.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

#include "core/text.h"

extern "C" {
#include "rc_version.h"
}

namespace core {
namespace {

struct Schliesser {
    HINTERNET h = nullptr;
    ~Schliesser() { if (h) ::WinHttpCloseHandle(h); }
};

}

const char* UserAgent() {
    return "DREAM-RAW/1.0.0 (Windows) rcheevos/" RCHEEVOS_VERSION_STRING;
}

std::string UrlKodieren(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string aus;
    aus.reserve(s.size() * 3 / 2);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            aus.push_back((char)c);
        } else {
            aus.push_back('%');
            aus.push_back(hex[c >> 4]);
            aus.push_back(hex[c & 0x0F]);
        }
    }
    return aus;
}

bool UrlZerlegen(const std::string& url, bool& https, std::string& host, std::string& pfad) {
    size_t rest;
    if (url.compare(0, 8, "https://") == 0)      { https = true;  rest = 8; }
    else if (url.compare(0, 7, "http://") == 0)  { https = false; rest = 7; }
    else return false;
    const size_t schraeg = url.find('/', rest);
    host = url.substr(rest, schraeg == std::string::npos ? std::string::npos : schraeg - rest);
    pfad = schraeg == std::string::npos ? "/" : url.substr(schraeg);
    return !host.empty();
}

HttpAntwort HttpAnfrage(const std::string& url, const std::string& formular, int timeout_ms) {
    HttpAntwort a;
    bool https = true;
    std::string host, pfad;
    if (!UrlZerlegen(url, https, host, pfad)) { a.fehler = T("Ungültige URL"); return a; }

    Schliesser sitzung;
    sitzung.h = ::WinHttpOpen(NachWide(UserAgent()).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sitzung.h) { a.fehler = "WinHttpOpen fehlgeschlagen"; return a; }
    ::WinHttpSetTimeouts(sitzung.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    Schliesser verbindung;
    verbindung.h = ::WinHttpConnect(sitzung.h, NachWide(host).c_str(),
                                    https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!verbindung.h) { a.fehler = Fmt(T("Verbindung zu %1 fehlgeschlagen"), {host}); return a; }

    const bool post = !formular.empty();
    Schliesser anfrage;
    anfrage.h = ::WinHttpOpenRequest(verbindung.h, post ? L"POST" : L"GET",
                                     NachWide(pfad).c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     https ? WINHTTP_FLAG_SECURE : 0);
    if (!anfrage.h) { a.fehler = T("Anfrage konnte nicht angelegt werden"); return a; }

    const wchar_t* kopf = post ? L"Content-Type: application/x-www-form-urlencoded\r\n"
                               : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!::WinHttpSendRequest(anfrage.h, kopf, post ? (DWORD)-1 : 0,
                              post ? (LPVOID)formular.data() : WINHTTP_NO_REQUEST_DATA,
                              (DWORD)formular.size(), (DWORD)formular.size(), 0)) {
        char t[96];
        std::snprintf(t, sizeof(t), "%s", Fmt(T("Senden fehlgeschlagen (Fehler %1)"), {std::to_string(::GetLastError())}).c_str());
        a.fehler = t;
        return a;
    }
    if (!::WinHttpReceiveResponse(anfrage.h, nullptr)) {
        char t[96];
        std::snprintf(t, sizeof(t), "%s", Fmt(T("Keine Antwort (Fehler %1)"), {std::to_string(::GetLastError())}).c_str());
        a.fehler = t;
        return a;
    }

    DWORD status = 0, groesse = sizeof(status);
    ::WinHttpQueryHeaders(anfrage.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &groesse,
                          WINHTTP_NO_HEADER_INDEX);
    a.status = (int)status;

    for (;;) {
        DWORD verfuegbar = 0;
        if (!::WinHttpQueryDataAvailable(anfrage.h, &verfuegbar) || verfuegbar == 0) break;
        std::vector<char> puffer(verfuegbar);
        DWORD gelesen = 0;
        if (!::WinHttpReadData(anfrage.h, puffer.data(), verfuegbar, &gelesen)) break;
        a.koerper.append(puffer.data(), gelesen);
    }

    a.ok = (status >= 200 && status < 300);
    if (!a.ok) {
        char t[32];
        std::snprintf(t, sizeof(t), "HTTP %d", a.status);
        a.fehler = t;
    }
    return a;
}

}
