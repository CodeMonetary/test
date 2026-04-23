#pragma once

namespace wf {

struct Config {
    bool enabled       = true;
    bool debug_banner  = false;
    bool log           = true;
    int  toggle_vk     = 0x7A; // VK_F11
};

void config_load(const char* exe_dir, Config* out);
const char* config_exe_dir();

} // namespace wf
