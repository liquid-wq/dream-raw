// Settings.

#pragma once

#include <string>

namespace core {

struct Einstellungen {
    std::string ra_benutzer;
    std::string dreampi_host;
    std::string dreamshell_host;
    int dreamshell_port = 0;
    bool angemeldet_bleiben = false;
    std::string ra_token;
    std::string kabel_port;
    long long nachricht_gelesen = 0;
    std::string sprache;
};

std::wstring EinstellungsPfad();

bool EinstellungenLaden(const std::wstring& pfad, Einstellungen& e);
bool EinstellungenSpeichern(const std::wstring& pfad, const Einstellungen& e);

}
