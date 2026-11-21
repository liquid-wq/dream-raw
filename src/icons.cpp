// Game icons for the console menu and the start logo.

#include "icons.h"

#include "i18n.h"

#include <algorithm>
#include <windows.h>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#include "core/http.h"
#include "core/ra.h"
#include "core/text.h"

namespace icons {
namespace {

constexpr int kGrundR = 0x1E, kGrundG = 0x22, kGrundB = 0x28;

std::wstring Ordner() {
    const wchar_t* a = _wgetenv(L"APPDATA");
    if (!a) return L"";
    const std::wstring o = std::wstring(a) + L"\\DREAM-RAW";
    ::CreateDirectoryW(o.c_str(), nullptr);
    return o;
}

bool DateiLesen(const std::wstring& p, std::string& d) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    char b[8192];
    size_t n;
    d.clear();
    while ((n = std::fread(b, 1, sizeof(b), f)) > 0) d.append(b, n);
    std::fclose(f);
    return !d.empty();
}

void DateiSchreiben(const std::wstring& p, const std::string& d) {
    if (FILE* f = _wfopen(p.c_str(), L"wb")) {
        std::fwrite(d.data(), 1, d.size(), f);
        std::fclose(f);
    }
}

std::map<uint32_t, std::string> AdressenLaden(const std::wstring& datei) {
    std::map<uint32_t, std::string> m;
    std::string d;
    if (!DateiLesen(datei, d)) return m;
    size_t pos = 0;
    while (pos < d.size()) {
        size_t ende = d.find('\n', pos);
        if (ende == std::string::npos) ende = d.size();
        const std::string z = d.substr(pos, ende - pos);
        const size_t leer = z.find(' ');
        if (leer != std::string::npos) m[static_cast<uint32_t>(std::strtoul(z.c_str(), nullptr, 10))] = core::Getrimmt(z.substr(leer + 1));
        pos = ende + 1;
    }
    return m;
}

void AdressenSpeichern(const std::wstring& datei, const std::map<uint32_t, std::string>& m) {
    std::string d;
    for (const auto& e : m) d += std::to_string(e.first) + " " + e.second + "\n";
    DateiSchreiben(datei, d);
}

bool NachRgb565(const std::string& png, std::vector<uint16_t>& aus) {
    using namespace Gdiplus;
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!h) return false;
    std::memcpy(::GlobalLock(h), png.data(), png.size());
    ::GlobalUnlock(h);
    IStream* strom = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(h, TRUE, &strom))) { ::GlobalFree(h); return false; }
    bool ok = false;
    {
        Bitmap roh(strom);
        if (roh.GetLastStatus() == Ok) {
            const int k = core::kIconKante;
            Bitmap bild(k, k, PixelFormat32bppARGB);
            {
                Graphics g(&bild);
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.Clear(Color(255, kGrundR, kGrundG, kGrundB));
                g.DrawImage(&roh, 0, 0, k, k);
            }
            BitmapData daten;
            Rect r(0, 0, k, k);
            if (bild.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &daten) == Ok) {
                aus.resize(static_cast<size_t>(k) * k);
                for (int y = 0; y < k; ++y) {
                    const uint8_t* z = static_cast<const uint8_t*>(daten.Scan0) + y * daten.Stride;
                    for (int x = 0; x < k; ++x) {
                        const uint8_t b = z[x * 4], gr = z[x * 4 + 1], rt = z[x * 4 + 2];
                        aus[static_cast<size_t>(y) * k + x] =
                            static_cast<uint16_t>(((rt >> 3) << 11) | ((gr >> 2) << 5) | (b >> 3));
                    }
                }
                bild.UnlockBits(&daten);
                ok = true;
            }
        }
    }
    strom->Release();
    return ok;
}

}

std::vector<core::GdiDatei> IconsFuerMenue(const std::vector<core::KartenPlatz>& plaetze, const std::string& benutzer,
                                           const std::string& token, std::vector<std::string>& meldungen,
                                           const std::function<void(const std::string&)>& fortschritt) {
    Gdiplus::GdiplusStartupInput ein;
    ULONG_PTR gdiplus = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus, &ein, nullptr) != Gdiplus::Ok) return {};

    const std::wstring ordner = Ordner();
    const std::wstring adressdatei = ordner + L"\\spiel_icons.txt";
    const std::wstring bildordner = ordner + L"\\icon_cache";
    ::CreateDirectoryW(bildordner.c_str(), nullptr);
    std::map<uint32_t, std::string> adressen = AdressenLaden(adressdatei);
    bool adressen_neu = false;
    auto letzte = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    auto drosseln = [&]() {
        const auto seit = std::chrono::steady_clock::now() - letzte;
        if (seit < std::chrono::milliseconds(1100)) std::this_thread::sleep_for(std::chrono::milliseconds(1100) - seit);
        letzte = std::chrono::steady_clock::now();
    };

    std::vector<core::IconBild> bilder;
    int ohne_anmeldung = 0, fehler = 0;
    for (const core::KartenPlatz& k : plaetze) {
        if (k.ra != '1' || !k.spiel_id) continue;
        if (fortschritt) fortschritt(Fmt(T("Schritt 3/5: RA-Icon für Platz %1 ..."), {std::to_string(k.platz)}));
        std::string png;
        const std::wstring bilddatei = bildordner + L"\\" + std::to_wstring(k.spiel_id) + L".png";
        if (!DateiLesen(bilddatei, png)) {
            auto it = adressen.find(k.spiel_id);
            if (it == adressen.end()) {
                if (token.empty()) { ++ohne_anmeldung; continue; }
                drosseln();
                const core::SpielDaten s = core::SpielLaden(benutzer, token, k.spiel_id);
                if (!s.ok || s.icon_url.empty()) { ++fehler; continue; }
                it = adressen.emplace(k.spiel_id, s.icon_url).first;
                adressen_neu = true;
            }
            drosseln();
            const core::HttpAntwort h = core::HttpAnfrage(it->second, "", 8000);
            if (!h.ok || h.status != 200 || h.koerper.size() < 8 || h.koerper.compare(1, 3, "PNG") != 0) { ++fehler; continue; }
            png = h.koerper;
            DateiSchreiben(bilddatei, png);
        }
        core::IconBild b;
        b.platz = k.platz;
        if (NachRgb565(png, b.rgb565)) bilder.push_back(std::move(b));
        else ++fehler;
    }
    if (adressen_neu) AdressenSpeichern(adressdatei, adressen);
    Gdiplus::GdiplusShutdown(gdiplus);

    std::string m = Fmt(T("RA-Icons: %1 im Menü"), {std::to_string(bilder.size())});
    if (ohne_anmeldung) m += Fmt(T(", %1 fehlen (zuerst bei RA anmelden)"), {std::to_string(ohne_anmeldung)});
    if (fehler) m += Fmt(T(", %1 nicht ladbar"), {std::to_string(fehler)});
    meldungen.push_back(m);
    if (bilder.empty()) return {};
    return {{"ICONS.BIN", core::IconsBinBauen(bilder)}};
}

std::vector<uint8_t> StartlogoMr() {
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartupInput ein;
    if (Gdiplus::GdiplusStartup(&gdiplus, &ein, nullptr) != Gdiplus::Ok) return {};
    constexpr int W = 320, H = 64;
    std::vector<uint8_t> index(static_cast<size_t>(W) * H, 0);

    const uint32_t dunkel = 0x3C4552, orange = 0xE4552C;
    std::vector<uint32_t> pal = {0xC0C0C0};
    auto mische = [](uint32_t z, float a) {
        auto k = [&](int s) { return static_cast<uint32_t>(255.0f + (static_cast<float>((z >> s) & 0xFF) - 255.0f) * a + 0.5f); };
        return (k(16) << 16) | (k(8) << 8) | k(0);
    };
    for (int i = 1; i <= 16; ++i) pal.push_back(mische(dunkel, i / 16.0f));
    for (int i = 1; i <= 16; ++i) pal.push_back(mische(orange, i / 16.0f));
    {
        Gdiplus::Bitmap bmp(W, H, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&bmp);
            g.Clear(Gdiplus::Color(255, 255, 255, 255));
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            Gdiplus::FontFamily fam(L"Segoe UI Semibold");
            Gdiplus::Font f(fam.IsAvailable() ? L"Segoe UI Semibold" : L"Arial", 44.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat sf;
            const wchar_t* t1 = L"DC-";
            const wchar_t* t2 = L"RAW";
            Gdiplus::RectF r1, r2;
            g.MeasureString(t1, -1, &f, Gdiplus::PointF(0, 0), Gdiplus::StringFormat::GenericTypographic(), &r1);
            g.MeasureString(t2, -1, &f, Gdiplus::PointF(0, 0), Gdiplus::StringFormat::GenericTypographic(), &r2);
            const float x0 = (W - r1.Width - r2.Width) / 2.0f, y0 = (H - r1.Height) / 2.0f;
            Gdiplus::SolidBrush b1(Gdiplus::Color(255, 0x3C, 0x45, 0x52)), b2(Gdiplus::Color(255, 0xE4, 0x55, 0x2C));
            g.DrawString(t1, -1, &f, Gdiplus::PointF(x0, y0), Gdiplus::StringFormat::GenericTypographic(), &b1);
            g.DrawString(t2, -1, &f, Gdiplus::PointF(x0 + r1.Width, y0), Gdiplus::StringFormat::GenericTypographic(), &b2);
        }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                Gdiplus::Color c;
                bmp.GetPixel(x, y, &c);
                const int r = c.GetR(), gg = c.GetG(), bb = c.GetB();
                if (r > 248 && gg > 248 && bb > 248) continue;
                int best = 1;
                long bestd = 1L << 30;
                for (int i = 1; i < static_cast<int>(pal.size()); ++i) {
                    const long dr = r - static_cast<int>((pal[i] >> 16) & 0xFF), dg = gg - static_cast<int>((pal[i] >> 8) & 0xFF),
                               db = bb - static_cast<int>(pal[i] & 0xFF);
                    const long d = dr * dr + dg * dg + db * db;
                    if (d < bestd) { bestd = d; best = i; }
                }
                index[static_cast<size_t>(y) * W + x] = static_cast<uint8_t>(best);
            }
    }
    Gdiplus::GdiplusShutdown(gdiplus);
    return core::MrKodieren(index, W, H, pal);
}
}
