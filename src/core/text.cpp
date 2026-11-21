// Text helpers.

#include "core/text.h"

#include <windows.h>

#include <cstdlib>

namespace core {

std::wstring NachWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string NachUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                        nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string Getrimmt(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

int PortLesen(const std::string& text) {
    const std::string t = Getrimmt(text);
    if (t.empty() || t.size() > 5) return -1;
    for (char c : t)
        if (c < '0' || c > '9') return -1;
    const int p = std::atoi(t.c_str());
    return (p >= 1 && p <= 65535) ? p : -1;
}

}
