// Unlock notification at the edge of the screen.

#pragma once

#include <functional>
#include <string>

namespace popup {

void Zeigen(const std::string& titel, const std::string& beschreibung, int punkte,
            const std::string& badgePng, const std::string& kopf, const std::string& einheit,
            float skalierung, std::function<void()> beimStart = nullptr);

bool Lage(int* x, int* y, int* w, int* h);

void Beenden();

}
