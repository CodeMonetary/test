#include "logger.h"
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace cod4mirror::log
{
	namespace
	{
		CRITICAL_SECTION g_cs;
		bool             g_cs_init = false;
		HANDLE           g_file    = INVALID_HANDLE_VALUE;
		bool             g_open_attempted = false;

		// Resolve absolute path "<game-dir>\cod4mirror.log" using the
		// directory of THIS dll (so the log lands next to d3d9.dll, which is
		// in the iw3mp.exe directory).
		void build_log_path(char* out, size_t out_size)
		{
			HMODULE self = nullptr;
			GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&build_log_path), &self);
			char path[MAX_PATH] = {0};
			GetModuleFileNameA(self, path, MAX_PATH);
			char* slash = std::strrchr(path, '\\');
			if (slash) *(slash + 1) = '\0';
			else path[0] = '\0';
			std::snprintf(out, out_size, "%scod4mirror.log", path);
		}

		void ensure_cs()
		{
			if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = true; }
		}

		void open_if_needed(bool truncate)
		{
			if (g_file != INVALID_HANDLE_VALUE) return;
			if (g_open_attempted && !truncate) return;
			g_open_attempted = true;
			char path[MAX_PATH];
			build_log_path(path, sizeof(path));
			DWORD disp = truncate ? CREATE_ALWAYS : OPEN_ALWAYS;
			g_file = CreateFileA(path, FILE_APPEND_DATA | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (g_file == INVALID_HANDLE_VALUE) return;
			if (!truncate) SetFilePointer(g_file, 0, nullptr, FILE_END);
		}

		void write_raw(const char* msg, size_t len)
		{
			if (g_file == INVALID_HANDLE_VALUE) return;
			DWORD wrote = 0;
			WriteFile(g_file, msg, (DWORD)len, &wrote, nullptr);
			FlushFileBuffers(g_file);
		}
	}

	void reset()
	{
		ensure_cs();
		EnterCriticalSection(&g_cs);
		if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
		g_open_attempted = false;
		open_if_needed(true);
		const char* hdr = "[cod4mirror] log opened\r\n";
		write_raw(hdr, std::strlen(hdr));
		OutputDebugStringA(hdr);
		LeaveCriticalSection(&g_cs);
	}

	void line(const char* fmt, ...)
	{
		ensure_cs();
		EnterCriticalSection(&g_cs);
		open_if_needed(false);

		char buf[1024];
		va_list ap; va_start(ap, fmt);
		int n = std::vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
		va_end(ap);
		if (n < 0) n = 0;
		if ((size_t)n > sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
		buf[n++] = '\r';
		buf[n++] = '\n';
		buf[n]   = '\0';

		write_raw(buf, (size_t)n);
		OutputDebugStringA(buf);
		LeaveCriticalSection(&g_cs);
	}
}
