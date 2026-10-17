#pragma once

#include <filesystem>
#include <string>

namespace dcp {

// Empty strings ask for the default.
struct PathRequest {
    std::string boot;
    std::string content_profile;
    std::string app0;
    std::string user_dir;
    std::string logs_dir;
};

struct Paths {
    std::filesystem::path boot;
    std::filesystem::path content_profile;
    std::filesystem::path app0;
    std::filesystem::path image_dir;
    std::filesystem::path user_dir;
    std::filesystem::path logs_dir;
};

// Explicit paths win. The rest are next to the executable: game\prepared\<build>, game\image\<build>, user and logs; or,
// for an executable in build\cmake\<toolchain>\bin, the repository's game\prepared\<build>, build\image\<build>, user
// and build\logs. <build> is the active build's folder (ActiveBuildPaths).
Paths ResolvePaths(const PathRequest& request, const std::filesystem::path& exe_dir);

} // namespace dcp
