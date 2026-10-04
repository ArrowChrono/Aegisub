// Copyright (c) 2026

#pragma once

#ifdef _WIN32
#include <windows.h>
#elif !defined(__APPLE__)
#include <GL/glx.h>
// Xlib's None macro conflicts with scoped enum values in application headers.
#ifdef None
#undef None
#endif
#else
#include <dlfcn.h>
#endif

namespace opengl {

inline void *GetProcAddress(char const *name) {
#ifdef _WIN32
	void *proc = reinterpret_cast<void *>(wglGetProcAddress(name));
	if (proc == nullptr
		|| proc == reinterpret_cast<void *>(0x1)
		|| proc == reinterpret_cast<void *>(0x2)
		|| proc == reinterpret_cast<void *>(0x3)
		|| proc == reinterpret_cast<void *>(-1))
		return nullptr;
	return proc;
#elif defined(__APPLE__)
	return dlsym(RTLD_DEFAULT, name);
#else
	return reinterpret_cast<void *>(glXGetProcAddress(reinterpret_cast<unsigned char const *>(name)));
#endif
}

}
