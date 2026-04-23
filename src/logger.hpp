#pragma once

namespace wf {

void log_init(const char* exe_dir);
void log_shutdown();
void logf(const char* fmt, ...);

} // namespace wf
