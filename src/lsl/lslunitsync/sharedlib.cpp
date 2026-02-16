/* This file is part of the Springlobby (GPL v2 or later), see COPYING */

#ifndef LSL_FUNCTION_PTR_H
#define LSL_FUNCTION_PTR_H

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <string>
#include "lslutils/logging.h"
#ifdef _WIN32
#include <windows.h>
#include <lslutils/conversion.h>
#include <lslutils/misc.h>
#else
#include <dlfcn.h>
#if defined(__GLIBC__)
#include <link.h>
#endif
#include <mutex>
#include <unordered_map>
#include <vector>
#endif

namespace LSL
{

#ifndef _WIN32
namespace
{
std::mutex g_extraHandlesMutex;
std::unordered_map<void*, std::vector<void*>> g_extraHandlesByPrimary;

void TrackExtraHandle(void* primary, void* extra)
{
	if (primary == nullptr || extra == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_extraHandlesMutex);
	g_extraHandlesByPrimary[primary].push_back(extra);
}

void CloseExtraHandles(void* primary)
{
	if (primary == nullptr) {
		return;
	}
	std::vector<void*> extras;
	{
		std::lock_guard<std::mutex> lock(g_extraHandlesMutex);
		auto it = g_extraHandlesByPrimary.find(primary);
		if (it == g_extraHandlesByPrimary.end()) {
			return;
		}
		extras = std::move(it->second);
		g_extraHandlesByPrimary.erase(it);
	}
	for (void* extra : extras) {
		if (extra != nullptr) {
			dlclose(extra);
		}
	}
}

} // namespace
#endif

void _FreeLibrary(void* handle)
{
	if (handle == nullptr)
		return;
#ifdef _WIN32
	FreeLibrary((HMODULE)handle);
#else
	dlclose(handle);
	CloseExtraHandles(handle);
#endif
}

void* _LoadLibrary(const std::string& libpath)
{
	void* res = nullptr;
#ifdef _WIN32
	const std::wstring wparentpath = Util::s2ws(LSL::Util::ParentPath(libpath));
	const std::wstring wlibpath = Util::s2ws(libpath);
	SetDllDirectory(nullptr);
	SetDllDirectory(wparentpath.c_str());
	res = LoadLibrary(wlibpath.c_str());
	if (res == nullptr) {
		LslError("Couldn't load the library %s: %s", libpath.c_str(), Util::geterrormsg().c_str());
		return res;
	}
#else
	int flags = RTLD_NOW | RTLD_LOCAL;
	// Prefer dlmopen() where available to isolate unitsync (and its bundled libs) in a new linker namespace.
	// This avoids symbol/ABI clashes that can crash the host process on some distros/toolchains.
#if defined(__GLIBC__) && defined(LM_ID_NEWLM)
	{
		LslDebug("Attempting to load unitsync via dlmopen(): %s", libpath.c_str());
		// Load unitsync in a fresh linker namespace to avoid symbol/ABI clashes with the host process.
		// Use RTLD_LAZY so that missing optional symbols (e.g. BAR missing DT_NEEDED for liblzma) don't abort the load.
		// Note: RTLD_GLOBAL is rejected by glibc's dlmopen() on some setups ("invalid mode"), so keep it local.
		const int dlmFlags = RTLD_LAZY | RTLD_LOCAL;
		dlerror(); // clear
		res = dlmopen(LM_ID_NEWLM, libpath.c_str(), dlmFlags);
		if (res != nullptr) {
			Lmid_t lmid = LM_ID_BASE;
			if (dlinfo(res, RTLD_DI_LMID, &lmid) == 0) {
				// BAR 2025.* bundles ship a libunitsync.so which references liblzma symbols (e.g. lzma_code) without a
				// DT_NEEDED entry; keep liblzma open in the same namespace for unitsync's lifetime.
				dlerror(); // clear
				void* lzmaHandle = dlmopen(lmid, "liblzma.so.5", RTLD_NOW | RTLD_GLOBAL);
				if (lzmaHandle != nullptr) {
					TrackExtraHandle(res, lzmaHandle);
				} else {
					dlerror(); // clear; liblzma isn't required for all engines
				}
			} else {
				dlerror(); // clear
			}

			LslDebug("Loaded unitsync via dlmopen() in isolated namespace: %s", libpath.c_str());
			return res;
		}
		const char* dlmErr = dlerror();
		LslWarning("dlmopen() failed for %s, falling back to dlopen(): %s", libpath.c_str(), dlmErr ? dlmErr : "");
		dlerror(); // clear any dlmopen errors; we'll fall back to regular dlopen().
	}
#endif

	res = dlopen(libpath.c_str(), flags);
	if (res == nullptr) {
		const char* errmsg = dlerror();
		const std::string err = errmsg ? errmsg : "";
		// BAR 2025.* bundles ship a libunitsync.so which references liblzma symbols
		// (e.g. lzma_code) without a DT_NEEDED entry; attempt a targeted workaround.
		if (err.find("undefined symbol: lzma_code") != std::string::npos || err.find("lzma_code") != std::string::npos) {
			dlerror(); // clear
			void* lzma = dlopen("liblzma.so.5", RTLD_NOW | RTLD_GLOBAL);
			if (lzma == nullptr) {
				const char* lzmaErr = dlerror();
				LslWarning("Couldn't preload liblzma.so.5: %s", lzmaErr ? lzmaErr : "");
			} else {
				LslInfo("Preloaded liblzma.so.5 for unitsync dlopen workaround");
			}

			dlerror(); // clear
			int retryFlags = RTLD_NOW | RTLD_GLOBAL;
			res = dlopen(libpath.c_str(), retryFlags);
			if (res != nullptr) {
				return res;
			}
			const char* retryErr = dlerror();
			LslError("Couldn't load the library %s after preloading liblzma.so.5: %s", libpath.c_str(), retryErr ? retryErr : "");
			return nullptr;
		}

		LslError("Couldn't load the library %s: %s", libpath.c_str(), errmsg);
		return nullptr;
	}
#endif
	return res;
}

void* GetLibFuncPtr(void* libhandle, const std::string& name)
{
	if (libhandle == nullptr)
		return nullptr;

#if defined _WIN32
	void* p = (void*)GetProcAddress((HMODULE)libhandle, name.c_str());
#else  // defined _WIN32
	void* p = dlsym(libhandle, name.c_str());
#endif // else defined _WIN32

	if (p == nullptr) {
		LslWarning("Couldn't load %s from unitsync library", name.c_str());
	}
	return p;
}


} // namespace LSL

#endif // LSL_FUNCTION_PTR_H
