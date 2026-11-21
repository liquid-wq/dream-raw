// HTTP requests to RetroAchievements.

#pragma once

#include <string>

namespace core {

struct HttpAntwort {
    bool ok = false;
    int status = 0;
    std::string koerper;
    std::string fehler;
};

const char* UserAgent();

HttpAntwort HttpAnfrage(const std::string& url, const std::string& formular,
                        int timeout_ms = 12000);

bool UrlZerlegen(const std::string& url, bool& https, std::string& host, std::string& pfad);

std::string UrlKodieren(const std::string& s);

}
