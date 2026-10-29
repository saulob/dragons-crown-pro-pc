#include "game_process.h"

#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "dcp_launcher.h"

namespace dcp {
namespace {

bool ChoosesLauncher(const std::string& argument) {
    return argument == "--launcher" || argument.rfind("--dcp-skip-launcher", 0) == 0 || argument == kGameProcessArgument;
}

#ifdef _WIN32
std::wstring Wide(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
    }
    return wide;
}

// CommandLineToArgvW's rules: backslashes are literal unless they precede a quote.
std::wstring Quote(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        quoted.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        quoted += c;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

PROCESS_INFORMATION g_game{};
#endif

} // namespace

bool SeparateGameProcessWanted(bool overlays_allowed, bool available) {
    return available && overlays_allowed;
}

std::vector<std::string> GameProcessArguments(const std::vector<std::string>& launcher_arguments,
                                              const std::string& log_file) {
    std::vector<std::string> arguments;
    for (size_t i = 0; i < launcher_arguments.size(); ++i) {
        if (launcher_arguments[i] == "--log") {
            ++i;
        } else if (!ChoosesLauncher(launcher_arguments[i])) {
            arguments.push_back(launcher_arguments[i]);
        }
    }
    arguments.emplace_back(kGameProcessArgument);
    if (!log_file.empty()) {
        arguments.emplace_back("--log");
        arguments.push_back(log_file);
    }
    return arguments;
}

#ifdef _WIN32
std::wstring WindowsCommandLine(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
    std::wstring command_line = Quote(executable.wstring());
    for (const std::string& argument : arguments) {
        command_line += L' ';
        command_line += Quote(Wide(argument));
    }
    return command_line;
}

bool CreateGameProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                       std::string& error) {
    if (g_game.hProcess) {
        error = "The game process was already created.";
        return false;
    }
    std::wstring command_line = WindowsCommandLine(executable, arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        nullptr, &startup, &process)) {
        error = "Windows could not start it (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    g_game = process;
    std::printf("Launcher: game process %lu created\n", process.dwProcessId);
    return true;
}
#else
bool CreateGameProcess(const std::filesystem::path&, const std::vector<std::string>&, std::string& error) {
    error = "The game runs in the launcher's process on this system.";
    return false;
}
#endif

} // namespace dcp

extern "C" int dcp_resume_game_process(void) {
#ifdef _WIN32
    using dcp::g_game;
    if (!g_game.hProcess) {
        return 0;
    }
    const bool resumed = ResumeThread(g_game.hThread) != static_cast<DWORD>(-1);
    if (!resumed) {
        TerminateProcess(g_game.hProcess, 1);
    }
    CloseHandle(g_game.hThread);
    CloseHandle(g_game.hProcess);
    g_game = {};
    return resumed ? 1 : 0;
#else
    return 0;
#endif
}
