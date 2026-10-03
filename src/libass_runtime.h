#pragma once

#include <string>

extern "C" {
#include <ass/ass.h>
}

namespace libass::runtime {

struct Api {
#define AGI_LIBASS_FN(name) decltype(&::name) name = nullptr;
#include "libass/libass_functions.inc"
#undef AGI_LIBASS_FN
};

void EnsureLoaded();
bool IsAvailable() noexcept;
std::string GetLoadError();
std::string GetLoadedLibrary();
int GetLoadedVersion() noexcept;
Api const& GetApi();

}
