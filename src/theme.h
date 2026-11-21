// Look of the console housing and the shared widgets.

#pragma once

#include "imgui.h"

namespace theme {

constexpr ImU32 kGrund        = IM_COL32(0xE6, 0xE5, 0xE0, 0xFF);
constexpr ImU32 kLeiste       = IM_COL32(0xD8, 0xD7, 0xD1, 0xFF);
constexpr ImU32 kKarte        = IM_COL32(0xF3, 0xF2, 0xEE, 0xFF);
constexpr ImU32 kKartenrand   = IM_COL32(0xC6, 0xC5, 0xBE, 0xFF);
constexpr ImU32 kFeld         = IM_COL32(0xFD, 0xFC, 0xFA, 0xFF);
constexpr ImU32 kKnopf        = IM_COL32(0xDF, 0xDE, 0xD8, 0xFF);
constexpr ImU32 kKnopfHover   = IM_COL32(0xD4, 0xD3, 0xCC, 0xFF);
constexpr ImU32 kKnopfAktiv   = IM_COL32(0xC8, 0xC7, 0xC0, 0xFF);
constexpr ImU32 kSilberHell   = IM_COL32(0xF4, 0xF5, 0xF7, 0xFF);
constexpr ImU32 kSilberDunkel = IM_COL32(0x9A, 0x9D, 0xA3, 0xFF);
constexpr ImU32 kText         = IM_COL32(0x2B, 0x2C, 0x30, 0xFF);
constexpr ImU32 kLeise        = IM_COL32(0x69, 0x6A, 0x6F, 0xFF);
constexpr ImU32 kSehrLeise    = IM_COL32(0x9A, 0x9A, 0x9C, 0xFF);
constexpr ImU32 kAkzent       = IM_COL32(0xE4, 0x55, 0x2C, 0xFF);
constexpr ImU32 kAkzentHell   = IM_COL32(0xEF, 0x6D, 0x45, 0xFF);
constexpr ImU32 kAkzentDunkel = IM_COL32(0xC1, 0x43, 0x1E, 0xFF);
constexpr ImU32 kInfo         = IM_COL32(0x2F, 0x76, 0xB2, 0xFF);
constexpr ImU32 kGut          = IM_COL32(0x2C, 0x96, 0x58, 0xFF);
constexpr ImU32 kWarnung      = IM_COL32(0xB5, 0x78, 0x0C, 0xFF);
constexpr ImU32 kFehler       = IM_COL32(0xC8, 0x3D, 0x31, 0xFF);

extern ImFont* gNormal;
extern ImFont* gHalbfett;
extern ImFont* gMono;

constexpr float kGroesseText   = 16.0f;
constexpr float kGroesseKlein  = 13.5f;
constexpr float kGroesseTitel  = 26.0f;
constexpr float kGroesseZwisch = 18.0f;

void Apply(float dpi);
float Dpi();

void Seitenkopf(const char* titel, const char* unterzeile);

void KarteBeginn(const char* id, const char* titel = nullptr, float breite = 0.0f, float hoehe = 0.0f);
void KarteEnde();

enum class Zustand { Neutral, Gut, Warnung, Fehler, Info };

void Marke(const char* text, Zustand z);

bool KnopfHaupt(const char* text, bool aktiv = true);
bool KnopfNeben(const char* text, bool aktiv = true);

void Hinweis(const char* text);

void KreisSetzen(ImVec2 mitte, float radius);

void KreisText(const char* text);

bool Feld(const char* beschriftung, const char* id, char* puffer, size_t groesse,
          ImGuiInputTextFlags flags = 0, const char* platzhalter = nullptr);

void Emblem(ImDrawList* dl, ImVec2 mitte, float groesse, float phase);

void Drehkreis(float radius);

}
