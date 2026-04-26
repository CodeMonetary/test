// cod4mirror — file + DebugView logger.
//
// Writes to "cod4mirror.log" next to the d3d9.dll (i.e. in the iw3mp.exe
// directory) and ALSO emits OutputDebugStringA so DebugView still works.
// File is opened lazily on first log call, in append mode, line-buffered.
#pragma once

namespace cod4mirror::log
{
	// Write a single line. Newline is appended automatically. Thread-safe-ish
	// (single CS, fine for our use — only one render thread + DllMain).
	void line(const char* fmt, ...);

	// Truncate the log file (called once early so each game launch starts
	// fresh).
	void reset();
}
