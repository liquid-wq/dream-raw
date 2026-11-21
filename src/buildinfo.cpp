// Build number.

#include "buildinfo.h"

#include <cstdio>

namespace buildinfo {

const char* string() {
    static char buf[96];
    if (!buf[0]) {
        std::snprintf(buf, sizeof(buf), "Build %d (%s) %s %s",
                      kBuild, kBuildDate, __DATE__, __TIME__);
    }
    return buf;
}

}
