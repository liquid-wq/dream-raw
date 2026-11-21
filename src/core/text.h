// Text helpers.

#pragma once

#include <string>

namespace core {

std::wstring NachWide(const std::string& utf8);
std::string NachUtf8(const std::wstring& w);

int PortLesen(const std::string& text);

std::string Getrimmt(const std::string& s);

}
