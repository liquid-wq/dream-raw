// Translations.

#pragma once

#include <initializer_list>
#include <string>
#include <vector>

namespace i18n {

constexpr int kSprachen = 7;
struct SpracheInfo { const char* kennung; const char* name; };
const std::vector<SpracheInfo>& Sprachen();

void SpracheSetzen(const std::string& kennung);
const std::string& Sprache();
int SpracheIndex();

std::string SystemSprache();

bool Englisch();

const char* T(const char* deutsch);
std::string T(const std::string& deutsch);

std::string Fmt(const std::string& muster, std::initializer_list<std::string> werte);

}

using i18n::T;
using i18n::Fmt;
