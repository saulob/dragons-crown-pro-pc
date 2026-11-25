// The parity contract of the supported builds: every build-dependent feature of every build, through the production
// locators and installers, on synthetic executables. No game file is read.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace parity {

enum class Path { kStatic, kSignature, kShared, kNotNeeded };

const char* PathName(Path path);

struct ContractResult {
    int checks = 0, failures = 0;
    std::vector<std::string> features;
    // [build label][feature]
    std::map<std::string, std::map<std::string, Path>> paths;
    std::map<std::string, int> build_failures;
    std::map<std::string, int> feature_failures;
    std::vector<std::string> differences; // "feature|builds|reason"

    bool Covers(const std::string& feature) const {
        for (const std::string& name : features) {
            if (name == feature) return feature_failures.find(feature) == feature_failures.end();
        }
        return false;
    }
    bool Passes(const std::string& build) const {
        return build_failures.find(build) == build_failures.end() && build_failures.find("contract") == build_failures.end();
    }
};

ContractResult RunBuildContract();

} // namespace parity
