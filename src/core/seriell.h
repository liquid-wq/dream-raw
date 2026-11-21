// Serial cable.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

std::vector<std::string> Cp210xPortsSuchen();

class SeriellerPort {
public:
    SeriellerPort() = default;
    ~SeriellerPort();
    SeriellerPort(const SeriellerPort&) = delete;
    SeriellerPort& operator=(const SeriellerPort&) = delete;

    bool Oeffnen(const std::string& name, uint32_t baud, std::string& fehler);
    void Schliessen();
    bool Offen() const;

    int Lesen(uint8_t* ziel, size_t max);
    bool Schreiben(const uint8_t* daten, size_t anzahl);

    int Cts() const;

    bool Flusssteuerung(bool an);

    bool Rts(bool an);

    bool BreakErkannt();

private:
    void* handle_ = nullptr;
};

}
