set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)

set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "boost-locale"
   OR PORT STREQUAL "fftw3"
   OR PORT STREQUAL "hunspell"
   OR PORT STREQUAL "icu"
   OR PORT STREQUAL "iconv"
   OR PORT STREQUAL "matroska"
   OR PORT STREQUAL "skia"
   OR PORT STREQUAL "uchardet"
   OR PORT STREQUAL "wxwidgets" 
   OR PORT STREQUAL "xaudio2redist")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()

set(VCPKG_BUILD_TYPE release)

# MSVC 14.x maintains binary compatibility across servicing/toolset
# updates. Avoid invalidating the entire vcpkg binary cache whenever
# the hosted runner updates its compiler build.
set(VCPKG_DISABLE_COMPILER_TRACKING ON)

