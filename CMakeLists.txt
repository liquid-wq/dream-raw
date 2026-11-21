cmake_minimum_required(VERSION 3.20)
project(dream_raw CXX C RC)

set(CMAKE_CXX_STANDARD 17)
add_link_options(-s)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

set(BUILD_NR 163)
string(TIMESTAMP BUILD_DATUM "%Y-%m-%d")
configure_file(src/buildinfo.h.in "${CMAKE_BINARY_DIR}/erzeugt/buildinfo.h" @ONLY)

set(APP_ICON_PFAD "${CMAKE_SOURCE_DIR}/assets/app.ico")
set(JASON_PFAD "${CMAKE_SOURCE_DIR}/assets/jason_pixel.png")
configure_file(src/app.rc.in "${CMAKE_BINARY_DIR}/erzeugt/app.rc" @ONLY)

set(DREAMRAW_BIN "${CMAKE_SOURCE_DIR}/console/menu.bin")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DREAMRAW_BIN}")
if(EXISTS "${DREAMRAW_BIN}")
    file(READ "${DREAMRAW_BIN}" DREAMRAW_HEX HEX)
    file(SIZE "${DREAMRAW_BIN}" DREAMRAW_GROESSE)
    string(REGEX REPLACE "(..)" "0x\\1," DREAMRAW_BYTES "${DREAMRAW_HEX}")
else()
    set(DREAMRAW_BYTES "")
    set(DREAMRAW_GROESSE 0)
    message(STATUS "console/menu.bin not found -- the program cannot set up the SD card")
endif()
configure_file(src/erstlauf.cpp.in "${CMAKE_BINARY_DIR}/erzeugt/erstlauf.cpp" @ONLY)

set(DEPS_DIR"${CMAKE_SOURCE_DIR}/deps" CACHE PATH "Folder with imgui/ and rcheevos/")
set(IMGUI_DIR "${DEPS_DIR}/imgui")
set(RCHEEVOS_DIR "${DEPS_DIR}/rcheevos")

foreach(d "${IMGUI_DIR}/imgui.cpp" "${RCHEEVOS_DIR}/include/rc_client.h")
    if(NOT EXISTS "${d}")
        message(FATAL_ERROR "Missing: ${d}")
    endif()
endforeach()

set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp"
)

file(GLOB RCHEEVOS_SOURCES
    "${RCHEEVOS_DIR}/src/*.c"
    "${RCHEEVOS_DIR}/src/rcheevos/*.c"
    "${RCHEEVOS_DIR}/src/rapi/*.c"
    "${RCHEEVOS_DIR}/src/rhash/*.c"
)
list(FILTER RCHEEVOS_SOURCES EXCLUDE REGEX "rc_client_raintegration|rc_libretro")

set(CORE_SOURCES
    src/core/http.cpp
    src/core/einstellungen.cpp
    src/core/ra.cpp
    src/core/text.cpp
    src/core/dcprotokoll.cpp
    src/core/seriell.cpp
    src/core/sitzung.cpp
    src/core/hashcache.cpp
    src/core/gdiabbild.cpp
    src/core/adressliste.cpp
    src/i18n.cpp
)

set(RCHEEVOS_INCLUDES
    "${RCHEEVOS_DIR}/include"
    "${RCHEEVOS_DIR}/src"
    "${RCHEEVOS_DIR}/src/rcheevos"
    "${RCHEEVOS_DIR}/src/rhash"
)

add_executable(dream_raw WIN32
    src/main.cpp
    src/theme.cpp
    src/popup.cpp
    src/icons.cpp
    src/bilder.cpp
    src/buildinfo.cpp
    "${CMAKE_BINARY_DIR}/erzeugt/erstlauf.cpp"
    "${CMAKE_BINARY_DIR}/erzeugt/app.rc"
    ${CORE_SOURCES}
    ${IMGUI_SOURCES}
    ${RCHEEVOS_SOURCES}
)
target_include_directories(dream_raw PRIVATE
    src "${CMAKE_BINARY_DIR}/erzeugt" "${IMGUI_DIR}" "${IMGUI_DIR}/backends"
    ${RCHEEVOS_INCLUDES})
target_compile_definitions(dream_raw PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX RC_DISABLE_LUA)
target_link_libraries(dream_raw PRIVATE
    opengl32 gdi32 user32 shell32 comdlg32 dwmapi winhttp ole32 setupapi gdiplus winmm)

if(MINGW)
    target_link_options(dream_raw PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

set(AUSGABE_DIR "${CMAKE_SOURCE_DIR}/bin" CACHE PATH "Output folder")
set_target_properties(dream_raw PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AUSGABE_DIR}"
    OUTPUT_NAME "DREAM-RAW")
