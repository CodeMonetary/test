#pragma once

#include <windows.h>

namespace wf {

void log_init(const char* exe_dir);
void log_shutdown();
void logf(const char* fmt, ...);
void log_guid(const char* prefix, REFIID riid);
void log_loaded_modules();

} // namespace wf
