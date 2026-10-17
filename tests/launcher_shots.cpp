// Renders every launcher page into <out dir>/page_N_Name.png with a hidden window and exits, to check the layout
// without a person: dcp-launcher-shots <app0> <image dir> <out dir> [user seed dir]. Settings go to a temporary user
// folder that starts as a copy of the seed folder.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "dcp_launcher.h"

#ifndef _WIN32
#include <csetjmp>
#include <cstdint>
// Defined by the loader's runtime, which this tool does not link.
extern "C" {
std::uint64_t runtime_disabled_optimizations;
__thread sigjmp_buf* runtime_fault_recover;
}
#endif

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: %s <app0> <image dir> <out dir>\n", argv[0]);
        return 2;
    }
    namespace fs = std::filesystem;
    const fs::path out = fs::absolute(argv[3]);
    fs::create_directories(out);
    const fs::path user = fs::temp_directory_path() / "dcp_launcher_shots_user";
    fs::remove_all(user);
    fs::create_directories(user);
    if (argc == 5) {
        fs::copy(argv[4], user, fs::copy_options::recursive);
    }
    const std::string settings = (user / "dcp.ini").string();
#ifdef _WIN32
    _putenv_s("DCP_LAUNCHER_SHOTS", out.string().c_str());
#else
    setenv("DCP_LAUNCHER_SHOTS", out.string().c_str(), 1);
#endif
    const std::string user_dir = user.string();
    const DcpLauncherConfig config{argv[1], argv[2], user_dir.c_str(), settings.c_str(), nullptr, 0, 0, nullptr};
    const DcpLauncherResult result = dcp_launcher_run(&config);
    std::printf("launcher returned %s\n", result == DCP_LAUNCHER_PLAY ? "PLAY" : "QUIT");
    return 0;
}
