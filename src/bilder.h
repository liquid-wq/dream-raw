// Loading PNG images into textures.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bilder {

struct Textur {
    unsigned id = 0;
    int w = 0, h = 0;
    float u = 1.0f, v = 1.0f;
    bool ok() const { return id != 0; }
};

bool Start();
void Ende();

bool PngDekodieren(const void* daten, size_t n, int* w, int* h, std::vector<uint8_t>* rgba);

bool PngDekodierenSkaliert(const void* daten, size_t n, int zielHoehe, int* w, int* h,
                           std::vector<uint8_t>* rgba);
Textur PngHochladenSkaliert(const void* daten, size_t n, int zielHoehe);

Textur Hochladen(const uint8_t* rgba, int w, int h, bool pixelig);
Textur PngHochladen(const void* daten, size_t n, bool pixelig);
void Freigeben(Textur& t);

bool Ressource(int id, const void** daten, size_t* n);

}
