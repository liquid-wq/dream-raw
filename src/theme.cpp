// Look of the console housing and the shared widgets.

#include "theme.h"

#include "i18n.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>

namespace theme {

ImFont* gNormal = nullptr;
ImFont* gHalbfett = nullptr;
ImFont* gMono = nullptr;

namespace {

float g_dpi = 1.0f;

ImVec4 F(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImVec4 Mit(ImU32 c, float alpha) {
    ImVec4 v = F(c);
    v.w = alpha;
    return v;
}

ImFont* Lade(std::initializer_list<const char*> kandidaten) {
    char win[MAX_PATH] = {};
    ::GetWindowsDirectoryA(win, MAX_PATH);
    for (const char* k : kandidaten) {
        const std::string p = std::string(win) + "\\Fonts\\" + k;
        if (::GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(p.c_str(), kGroesseText)) return f;
    }
    return nullptr;
}

ImU32 Farbe(Zustand z) {
    switch (z) {
        case Zustand::Gut:     return kGut;
        case Zustand::Warnung: return kWarnung;
        case Zustand::Fehler:  return kFehler;
        case Zustand::Info:    return kInfo;
        default:               return kLeise;
    }
}

}

float Dpi() { return g_dpi; }

void Apply(float dpi) {
    g_dpi = dpi;

    gNormal   = Lade({"segoeui.ttf", "tahoma.ttf", "arial.ttf"});
    gHalbfett = Lade({"seguisb.ttf", "segoeuib.ttf", "arialbd.ttf"});
    gMono     = Lade({"CascadiaMono.ttf", "consola.ttf", "cour.ttf"});
    if (!gHalbfett) gHalbfett = gNormal;
    if (!gMono) gMono = gNormal;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.FontSizeBase = kGroesseText;
    s.FontScaleDpi = dpi;

    s.WindowPadding    = ImVec2(0, 0);
    s.FramePadding     = ImVec2(11, 7);
    s.ItemSpacing      = ImVec2(10, 9);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.CellPadding      = ImVec2(10, 6);
    s.ScrollbarSize    = 11;
    s.GrabMinSize      = 10;
    s.WindowRounding   = 0;
    s.ChildRounding    = 10;
    s.FrameRounding    = 7;
    s.PopupRounding    = 8;
    s.ScrollbarRounding = 6;
    s.GrabRounding     = 6;
    s.TabRounding      = 6;
    s.WindowBorderSize = 0;
    s.ChildBorderSize  = 1;
    s.FrameBorderSize  = 1;
    s.SeparatorTextBorderSize = 1;
    s.ScaleAllSizes(dpi);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = F(kText);
    c[ImGuiCol_TextDisabled]         = F(kSehrLeise);
    c[ImGuiCol_WindowBg]             = F(kGrund);
    c[ImGuiCol_ChildBg]              = Mit(kKarte, 0.0f);
    c[ImGuiCol_PopupBg]              = F(kKarte);
    c[ImGuiCol_Border]               = F(kKartenrand);
    c[ImGuiCol_BorderShadow]         = Mit(kGrund, 0.0f);
    c[ImGuiCol_FrameBg]              = F(kFeld);
    c[ImGuiCol_FrameBgHovered]       = F(IM_COL32(0xFF, 0xFF, 0xFF, 0xFF));
    c[ImGuiCol_FrameBgActive]        = F(IM_COL32(0xFF, 0xFF, 0xFF, 0xFF));
    c[ImGuiCol_TitleBg]              = F(kLeiste);
    c[ImGuiCol_TitleBgActive]        = F(kLeiste);
    c[ImGuiCol_TitleBgCollapsed]     = F(kLeiste);
    c[ImGuiCol_MenuBarBg]            = F(kLeiste);
    c[ImGuiCol_ScrollbarBg]          = Mit(kGrund, 0.0f);
    c[ImGuiCol_ScrollbarGrab]        = F(kKartenrand);
    c[ImGuiCol_ScrollbarGrabHovered] = F(kSilberDunkel);
    c[ImGuiCol_ScrollbarGrabActive]  = F(kLeise);
    c[ImGuiCol_CheckMark]            = F(kAkzent);
    c[ImGuiCol_SliderGrab]           = F(kAkzent);
    c[ImGuiCol_SliderGrabActive]     = F(kAkzentHell);
    c[ImGuiCol_Button]               = F(kKnopf);
    c[ImGuiCol_ButtonHovered]        = F(kKnopfHover);
    c[ImGuiCol_ButtonActive]         = F(kKnopfAktiv);
    c[ImGuiCol_Header]               = Mit(kAkzent, 0.12f);
    c[ImGuiCol_HeaderHovered]        = Mit(kAkzent, 0.18f);
    c[ImGuiCol_HeaderActive]         = Mit(kAkzent, 0.25f);
    c[ImGuiCol_Separator]            = F(kKartenrand);
    c[ImGuiCol_SeparatorHovered]     = F(kAkzent);
    c[ImGuiCol_SeparatorActive]      = F(kAkzentHell);
    c[ImGuiCol_ResizeGrip]           = Mit(kAkzent, 0.0f);
    c[ImGuiCol_ResizeGripHovered]    = Mit(kAkzent, 0.4f);
    c[ImGuiCol_ResizeGripActive]     = F(kAkzent);
    c[ImGuiCol_InputTextCursor]      = F(kAkzent);
    c[ImGuiCol_Tab]                  = F(kKnopf);
    c[ImGuiCol_TabHovered]           = F(kKnopfHover);
    c[ImGuiCol_TabSelected]          = F(kKarte);
    c[ImGuiCol_TableHeaderBg]        = F(kLeiste);
    c[ImGuiCol_TableBorderStrong]    = F(kKartenrand);
    c[ImGuiCol_TableBorderLight]     = Mit(kKartenrand, 0.7f);
    c[ImGuiCol_TableRowBg]           = Mit(kText, 0.0f);
    c[ImGuiCol_TableRowBgAlt]        = Mit(kText, 0.025f);
    c[ImGuiCol_TextLink]             = F(kInfo);
    c[ImGuiCol_TextSelectedBg]       = Mit(kAkzent, 0.25f);
    c[ImGuiCol_NavCursor]            = F(kAkzent);
    c[ImGuiCol_ModalWindowDimBg]     = Mit(kText, 0.35f);
}

static void Wulst(bool haupt) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float r = (b.y - a.y) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 glanz = haupt ? IM_COL32(0xFF, 0xFF, 0xFF, 0x40) : IM_COL32(0xFF, 0xFF, 0xFF, 0xB0);
    const ImU32 kante = haupt ? IM_COL32(0x80, 0x20, 0x08, 0x50) : IM_COL32(0x70, 0x70, 0x68, 0x55);
    dl->AddLine(ImVec2(a.x + r, a.y + 1.0f), ImVec2(b.x - r, a.y + 1.0f), glanz, 1.5f * g_dpi);
    dl->AddLine(ImVec2(a.x + r, b.y - 1.0f), ImVec2(b.x - r, b.y - 1.0f), kante, 1.0f * g_dpi);
}

void Seitenkopf(const char* titel, const char* unterzeile) {
    titel = T(titel);
    unterzeile = T(unterzeile);
    ImGui::PushFont(gHalbfett, kGroesseTitel);
    ImGui::TextUnformatted(titel);
    ImGui::PopFont();
    if (unterzeile && *unterzeile) {
        ImGui::PushStyleColor(ImGuiCol_Text, F(kLeise));
        ImGui::TextWrapped("%s", unterzeile);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 8 * g_dpi));
}

void KarteBeginn(const char* id, const char* titel, float breite, float hoehe) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, F(kKarte));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * g_dpi, 14 * g_dpi));
    ImGuiChildFlags f = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (hoehe <= 0.0f) f |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild(id, ImVec2(breite, hoehe), f,
                      hoehe <= 0.0f ? ImGuiWindowFlags_NoScrollbar : 0);
    {

        const ImVec2 a = ImGui::GetWindowPos();
        const float w = ImGui::GetWindowWidth();
        const float r = ImGui::GetStyle().ChildRounding;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + r, a.y + 1.5f), ImVec2(a.x + w - r, a.y + 1.5f),
                                            IM_COL32(0xFF, 0xFF, 0xFF, 0xD0), 1.5f * g_dpi);
    }
    if (titel) {
        ImGui::PushFont(gHalbfett, kGroesseZwisch);
        ImGui::TextUnformatted(titel);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2 * g_dpi));
    }
}

void KarteEnde() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Marke(const char* text, Zustand z) {
    text = T(text);
    const ImU32 farbe = Farbe(z);
    ImGui::PushFont(gHalbfett, kGroesseKlein);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float pad_x = 9 * g_dpi, pad_y = 3 * g_dpi, punkt = 3.5f * g_dpi;
    const ImVec2 groesse(ts.x + pad_x * 2 + punkt * 2 + 6 * g_dpi, ts.y + pad_y * 2);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 hg = F(farbe);
    hg.w = 0.14f;
    dl->AddRectFilled(p, ImVec2(p.x + groesse.x, p.y + groesse.y),
                      ImGui::ColorConvertFloat4ToU32(hg), groesse.y * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + pad_x + punkt, p.y + groesse.y * 0.5f), punkt, farbe);
    dl->AddText(ImVec2(p.x + pad_x + punkt * 2 + 6 * g_dpi, p.y + pad_y), farbe, text);
    ImGui::Dummy(groesse);
    ImGui::PopFont();
}

bool KnopfHaupt(const char* text, bool aktiv) {
    text = T(text);
    ImGui::BeginDisabled(!aktiv);
    ImGui::PushStyleColor(ImGuiCol_Button, F(kAkzent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, F(kAkzentHell));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, F(kAkzentDunkel));
    ImGui::PushStyleColor(ImGuiCol_Text, F(IM_COL32(0xFF, 0xFF, 0xFF, 0xFF)));
    ImGui::PushStyleColor(ImGuiCol_Border, F(kAkzentDunkel));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 100.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18 * g_dpi, 7 * g_dpi));
    ImGui::PushFont(gHalbfett, 0.0f);
    const bool r = ImGui::Button(text);
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    Wulst(true);
    ImGui::EndDisabled();
    return r;
}

bool KnopfNeben(const char* text, bool aktiv) {
    text = T(text);
    ImGui::BeginDisabled(!aktiv);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 100.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16 * g_dpi, 7 * g_dpi));
    const bool r = ImGui::Button(text);
    ImGui::PopStyleVar(2);
    Wulst(false);
    ImGui::EndDisabled();
    return r;
}

namespace {
ImVec2 g_kreis_m;
float g_kreis_r = 0.0f;
}

void KreisSetzen(ImVec2 mitte, float radius) {
    g_kreis_m = mitte;
    g_kreis_r = radius;
}

void KreisText(const char* text) {
    if (g_kreis_r <= 0.0f) { ImGui::TextWrapped("%s", text); return; }
    ImFont* f = ImGui::GetFont();
    const float gr = ImGui::GetFontSize();
    const float zeile = ImGui::GetTextLineHeightWithSpacing();
    const float wx0 = ImGui::GetCursorScreenPos().x;
    const float wx1 = wx0 + ImGui::GetContentRegionAvail().x;
    const float rand = 6.0f * g_dpi;
    const char* s = text;
    const char* ende = text + std::strlen(text);
    while (s < ende) {
        const float y = ImGui::GetCursorScreenPos().y;

        const float dy = std::fmax(std::fabs(y - g_kreis_m.y), std::fabs(y + zeile - g_kreis_m.y));
        const float halb = dy < g_kreis_r ? std::sqrt(g_kreis_r * g_kreis_r - dy * dy) - rand : 0.0f;
        const float x0 = std::fmax(wx0, g_kreis_m.x - halb), x1 = std::fmin(wx1, g_kreis_m.x + halb);
        const float breite = std::fmax(x1 - x0, 40.0f * g_dpi);
        const char* zeilenende = std::strchr(s, '\n');
        if (!zeilenende) zeilenende = ende;
        const char* e = f->CalcWordWrapPosition(gr, s, zeilenende, breite);
        if (e <= s) e = s + 1;
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        ImGui::TextUnformatted(s, e);
        s = e;
        while (s < ende && (*s == ' ' || *s == '\n')) ++s;
    }

    ImGui::SetCursorScreenPos(ImVec2(wx0, ImGui::GetCursorScreenPos().y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void Hinweis(const char* text) {
    text = T(text);
    ImGui::PushStyleColor(ImGuiCol_Text, F(kLeise));
    ImGui::PushFont(nullptr, kGroesseKlein + 0.5f);
    ImGui::TextWrapped("%s", text);
    ImGui::PopFont();
    ImGui::PopStyleColor();
}

bool Feld(const char* beschriftung, const char* id, char* puffer, size_t groesse,
          ImGuiInputTextFlags flags, const char* platzhalter) {
    beschriftung = T(beschriftung);
    platzhalter = T(platzhalter);
    ImGui::PushStyleColor(ImGuiCol_Text, F(kLeise));
    ImGui::PushFont(nullptr, kGroesseKlein + 0.5f);
    ImGui::TextUnformatted(beschriftung);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1.0f);
    if (platzhalter)
        return ImGui::InputTextWithHint(id, platzhalter, puffer, groesse, flags);
    return ImGui::InputText(id, puffer, groesse, flags);
}

void Emblem(ImDrawList* dl, ImVec2 m, float g, float phase) {

    const float r = g * 0.5f;
    dl->AddCircleFilled(m, r, kKnopfAktiv, 48);
    dl->AddCircleFilled(ImVec2(m.x, m.y - r * 0.04f), r * 0.90f, kKnopf, 48);
    dl->AddCircle(m, r, IM_COL32(0xFF, 0xFF, 0xFF, 0x90), 48, 1.2f * g_dpi);

    const ImVec2 o1(m.x - r * 0.46f, m.y - r * 0.18f);
    const ImVec2 o2(m.x + r * 0.46f, m.y - r * 0.18f);
    const ImVec2 u(m.x, m.y + r * 0.52f);
    dl->AddTriangleFilled(o1, o2, u, kSilberDunkel);

    dl->AddTriangleFilled(o1, ImVec2(m.x, m.y - r * 0.18f), u, kSilberHell);
    dl->AddTriangle(o1, o2, u, IM_COL32(0x70, 0x73, 0x78, 0xA0), 1.0f * g_dpi);

    const float puls = 0.75f + 0.25f * std::sin(phase * 2.0f);
    ImVec4 farbe = F(kAkzent);
    dl->AddCircleFilled(ImVec2(m.x, m.y - r * 0.50f), r * 0.13f * (0.9f + 0.1f * puls),
                        ImGui::ColorConvertFloat4ToU32(farbe), 24);
    farbe.w = 0.25f * puls;
    dl->AddCircleFilled(ImVec2(m.x, m.y - r * 0.50f), r * 0.24f, ImGui::ColorConvertFloat4ToU32(farbe), 24);
}

void Drehkreis(float radius) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float d = radius * 2.0f;
    ImGui::Dummy(ImVec2(d, d));
    const ImVec2 m(p.x + radius, p.y + radius);
    const float t = (float)ImGui::GetTime() * 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PathArcTo(m, radius * 0.8f, t, t + 4.2f, 24);
    dl->PathStroke(kAkzent, 0, radius * 0.28f);
}

}
