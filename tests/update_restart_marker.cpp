#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

std::string Utf8(const wchar_t* text) {
  const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) {
    return {};
  }
  std::string result(size_t(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
  result.resize(size_t(length - 1));
  return result;
}

bool Present(const wchar_t* name) {
  return GetEnvironmentVariableW(name, nullptr, 0) > 0;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  int count = 0;
  wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments) {
    return 2;
  }
  const bool hold = count >= 4 && std::wstring_view(arguments[1]) == L"--hold";
  const int output_index = hold ? 2 : 1;
  if (count <= output_index) {
    LocalFree(arguments);
    return 2;
  }
  std::ofstream out(std::filesystem::path(arguments[output_index]), std::ios::binary | std::ios::trunc);
  if (!out) {
    LocalFree(arguments);
    return 3;
  }
  out << "argc=" << count << '\n';
  for (int i = 1; i < count; ++i) {
    out << "arg" << i << '=' << Utf8(arguments[i]) << '\n';
  }
  out << "env_DCP_UPDATE_TEST=" << (Present(L"DCP_UPDATE_TEST") ? "present" : "absent") << '\n';
  out << "env_DCP_UPDATE_TEST_CURRENT_VERSION="
      << (Present(L"DCP_UPDATE_TEST_CURRENT_VERSION") ? "present" : "absent") << '\n';
  out << "env_DCP_UPDATE_TEST_RELEASE_JSON="
      << (Present(L"DCP_UPDATE_TEST_RELEASE_JSON") ? "present" : "absent") << '\n';
  out << "env_DCP_UPDATE_TEST_ASSET_SHA256="
      << (Present(L"DCP_UPDATE_TEST_ASSET_SHA256") ? "present" : "absent") << '\n';
  out << "done=1\n";
  if (hold) {
    Sleep(static_cast<DWORD>(std::wcstoul(arguments[2], nullptr, 10)));
  }
  LocalFree(arguments);
  return 0;
}
