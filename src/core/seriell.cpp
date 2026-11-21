// Serial cable.

#include "core/seriell.h"

#include "i18n.h"

#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>

#include <algorithm>
#include <cctype>

namespace core {

std::vector<std::string> Cp210xPortsSuchen() {
    std::vector<std::string> aus;
    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) return aus;
    SP_DEVINFO_DATA daten = {};
    daten.cbSize = sizeof(daten);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(info, i, &daten); ++i) {
        char hwid[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyA(info, &daten, SPDRP_HARDWAREID, nullptr,
                                               reinterpret_cast<PBYTE>(hwid), sizeof(hwid) - 2, nullptr))
            continue;
        std::string s = hwid;
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s.find("vid_10c4") == std::string::npos || s.find("pid_ea60") == std::string::npos) continue;
        HKEY key = SetupDiOpenDevRegKey(info, &daten, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        char name[64] = {};
        DWORD typ = 0, groesse = sizeof(name) - 1;
        const LONG r = RegQueryValueExA(key, "PortName", nullptr, &typ, reinterpret_cast<LPBYTE>(name), &groesse);
        RegCloseKey(key);
        if (r == ERROR_SUCCESS && name[0]) aus.push_back(name);
    }
    SetupDiDestroyDeviceInfoList(info);
    std::sort(aus.begin(), aus.end());
    return aus;
}

SeriellerPort::~SeriellerPort() { Schliessen(); }

bool SeriellerPort::Oeffnen(const std::string& name, uint32_t baud, std::string& fehler) {
    Schliessen();
    const std::string pfad = "\\\\.\\" + name;
    HANDLE h = CreateFileA(pfad.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        fehler = e == ERROR_ACCESS_DENIED ? Fmt(T("%1 ist von einem anderen Programm belegt"), {name})
               : e == ERROR_FILE_NOT_FOUND ? Fmt(T("%1 nicht gefunden (Kabel eingesteckt?)"), {name})
               : Fmt(T("%1 lässt sich nicht öffnen (Fehler %2)"), {name, std::to_string(e)});
        return false;
    }
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;

    dcb.fOutxCtsFlow = TRUE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        fehler = Fmt(T("%1: Baudrate %2 wird nicht angenommen"), {name, std::to_string(baud)});
        CloseHandle(h);
        return false;
    }
    SetupComm(h, 1 << 20, 1 << 16);
    COMMTIMEOUTS t = {};
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = MAXDWORD;
    t.ReadTotalTimeoutConstant = 50;
    t.WriteTotalTimeoutConstant = 500;
    SetCommTimeouts(h, &t);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    handle_ = h;
    return true;
}

void SeriellerPort::Schliessen() {
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
}

bool SeriellerPort::Offen() const { return handle_ != nullptr; }

int SeriellerPort::Lesen(uint8_t* ziel, size_t max) {
    if (!handle_) return -1;
    DWORD n = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), ziel, static_cast<DWORD>(max), &n, nullptr)) return -1;
    return static_cast<int>(n);
}

bool SeriellerPort::Schreiben(const uint8_t* daten, size_t anzahl) {
    if (!handle_) return false;
    DWORD n = 0;
    return WriteFile(static_cast<HANDLE>(handle_), daten, static_cast<DWORD>(anzahl), &n, nullptr) && n == anzahl;
}

int SeriellerPort::Cts() const {
    if (!handle_) return -1;
    DWORD st = 0;
    if (!GetCommModemStatus(static_cast<HANDLE>(handle_), &st)) return -1;
    return (st & MS_CTS_ON) ? 1 : 0;
}

bool SeriellerPort::Flusssteuerung(bool an) {
    if (!handle_) return false;
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(static_cast<HANDLE>(handle_), &dcb)) return false;
    dcb.fOutxCtsFlow = an ? TRUE : FALSE;
    dcb.fRtsControl = an ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
    return SetCommState(static_cast<HANDLE>(handle_), &dcb) != 0;
}

bool SeriellerPort::Rts(bool an) {
    if (!handle_) return false;
    return EscapeCommFunction(static_cast<HANDLE>(handle_), an ? SETRTS : CLRRTS) != 0;
}

bool SeriellerPort::BreakErkannt() {
    if (!handle_) return false;
    DWORD fehler = 0;
    COMSTAT st{};
    if (!ClearCommError(static_cast<HANDLE>(handle_), &fehler, &st)) return false;
    return (fehler & CE_BREAK) != 0;
}

}
