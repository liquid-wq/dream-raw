// SD card: game list, menu image, backup and restore.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <filesystem>
#include <string>
#include <filesystem>
#include <vector>
#include <filesystem>

namespace core {

struct GdiDatei {
    std::string name;
    std::vector<uint8_t> daten;
};

bool GdiSchreiben(const std::wstring& ordner, const std::vector<uint8_t>& ipbin,
                  std::vector<GdiDatei> dateien, std::string& fehler);

bool IpBinAusGdi(const std::wstring& gdi_pfad, std::vector<uint8_t>& ipbin, std::string& fehler);

void IpBinAnpassen(std::vector<uint8_t>& ipbin, const std::string& startdatei, const std::string& titel,
                   const std::string& produkt);

bool StartdateiAusGdi(const std::wstring& gdi_pfad, std::vector<uint8_t>& ip, std::vector<uint8_t>& datei,
                      std::string& name, std::string& fehler);
bool RaHashesAusGdi(const std::wstring& gdi_pfad, std::vector<std::string>& hashes, std::string& fehler);

struct KartenPlatz {
    int platz = 0;
    std::string format;
    std::string titel;
    std::wstring abbild;
    char ra = '?';
    bool eigenes_menue = false;
    uint32_t spiel_id = 0;
    bool platz_hoch = false;
};

int SpurenUmbenennen(const std::wstring& gdi_pfad, std::string& meldung);

int SpurenZuruecksetzen(const std::filesystem::path& ordner, std::string& meldung);

bool ZweiterPlatzNoetig(const std::string& ip_titel);

constexpr size_t kMrOffset = 0x3820, kMrMax = 8192;
std::vector<uint8_t> MrKodieren(const std::vector<uint8_t>& index, int w, int h, const std::vector<uint32_t>& palette_rgb);

bool IpLogoEinsetzen(std::vector<uint8_t>& ipbin, const std::vector<uint8_t>& mr);

void StartlogoVorgeben(std::vector<uint8_t> mr);

std::vector<KartenPlatz> KarteLesen(const std::wstring& wurzel);

struct RaPruefung {
    int bekannt = 0, unbekannt = 0, offen = 0, abfragen = 0;
    bool abgebrochen = false;
    std::string fehler;
};
RaPruefung RaStatusBestimmen(std::vector<KartenPlatz>& plaetze, bool netz, const volatile bool* abbruch,
                             const std::function<void(const std::string&)>& meldung);

constexpr int kIconKante = 96;
struct IconBild {
    int platz = 0;
    std::vector<uint16_t> rgb565;
};
std::vector<uint8_t> IconsBinBauen(const std::vector<IconBild>& icons);

std::string SpielelisteText(const std::vector<KartenPlatz>& plaetze, int menue_platz);

struct MenueErgebnis {
    bool ok = false;
    std::string fehler;
    int spiele = 0;
    std::vector<std::string> meldungen;
};

using ZusatzDateien = std::function<std::vector<GdiDatei>(const std::vector<KartenPlatz>&,
                                                          std::vector<std::string>& meldungen)>;
MenueErgebnis MenueAufKarte(const std::wstring& wurzel, int platz, const std::vector<uint8_t>& erstlauf,
                            bool ra_status = true, const ZusatzDateien& zusatz = nullptr,
                            const std::function<void(const std::string&)>& fortschritt = nullptr);

bool SicherungVorhanden(const std::wstring& wurzel, int platz);
MenueErgebnis OriginalWiederherstellen(const std::wstring& wurzel, int platz,
                                       const std::function<void(const std::string&)>& fortschritt = nullptr);

}
