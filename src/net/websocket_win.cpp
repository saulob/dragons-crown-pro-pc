// WebSocket client on WinHTTP: system proxy settings and certificate store, TLS by Windows.
#include "net/websocket.h"

#include <mutex>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

namespace dcp::net {
namespace {

std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

std::string Describe(const char* step, DWORD error) {
    switch (error) {
    case ERROR_WINHTTP_TIMEOUT: return std::string(step) + ": timed out";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the lobby's host name does not resolve";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "the lobby refused the connection or is not running";
    case ERROR_WINHTTP_SECURE_FAILURE: return "the lobby's TLS certificate was not accepted";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection to the lobby was reset";
    default: return std::string(step) + " failed (error " + std::to_string(error) + ")";
    }
}

class WinHttpSocket final : public WebSocket {
public:
    WinHttpSocket(HINTERNET session, HINTERNET connection, HINTERNET socket)
        : session_(session), connection_(connection), socket_(socket) {}

    ~WinHttpSocket() override {
        close();
        std::lock_guard<std::mutex> lock(mutex_);
        if (socket_) WinHttpCloseHandle(socket_);
        WinHttpCloseHandle(connection_);
        WinHttpCloseHandle(session_);
    }

    bool send_text(const std::string& text) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!socket_ || closed_) return false;
        return WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    const_cast<char*>(text.data()), DWORD(text.size())) == NO_ERROR;
    }

    bool receive(std::string& text) override {
        text.clear();
        std::vector<char> buffer(16 * 1024);
        for (;;) {
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            HINTERNET socket;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_ || !socket_) return false;
                socket = socket_;
            }
            if (WinHttpWebSocketReceive(socket, buffer.data(), DWORD(buffer.size()), &read, &type) != NO_ERROR) return false;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return false;
            text.append(buffer.data(), read);
            if (text.size() > 4 * 1024 * 1024) return false;
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                return true;
            }
        }
    }

    void close() override {
        HINTERNET socket = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            closed_ = true;
            socket = socket_;
            socket_ = nullptr;
        }
        if (socket) {
            WinHttpWebSocketShutdown(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            // Closing the handle also ends a receive blocked on another thread.
            WinHttpCloseHandle(socket);
        }
    }

private:
    std::mutex mutex_;
    HINTERNET session_;
    HINTERNET connection_;
    HINTERNET socket_;
    bool closed_ = false;
};

} // namespace

std::unique_ptr<WebSocket> WebSocket::connect(const std::string& url, int timeout_ms, std::string* error) {
    WsUrl parts;
    if (!parse_ws_url(url, parts, error)) return nullptr;
    HINTERNET session = WinHttpOpen(L"DragonsCrownProPC", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(L"DragonsCrownProPC", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        if (error) *error = Describe("WinHTTP", GetLastError());
        return nullptr;
    }
    // Receives wait for the lobby's keepalive answers (every 20 s); the link's own watchdog ends a silent socket.
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, 75000);
    HINTERNET connection = WinHttpConnect(session, Wide(parts.host).c_str(), parts.port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", Wide(parts.path).c_str(), nullptr,
                                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                        parts.tls ? WINHTTP_FLAG_SECURE : 0)
                                   : nullptr;
    DWORD status = 0, size = sizeof(status);
    const bool upgraded = request && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                          WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                          WinHttpReceiveResponse(request, nullptr) &&
                          WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    const DWORD failure = upgraded ? 0 : GetLastError();
    HINTERNET socket = upgraded && status == 101 ? WinHttpWebSocketCompleteUpgrade(request, 0) : nullptr;
    if (request) WinHttpCloseHandle(request);
    if (!socket) {
        if (error) {
            if (!upgraded) *error = Describe("connecting to the lobby", failure);
            else if (status == 429) *error = "the lobby has too many connections from this address";
            else *error = "the lobby answered HTTP " + std::to_string(status) + " instead of a WebSocket";
        }
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return nullptr;
    }
    return std::make_unique<WinHttpSocket>(session, connection, socket);
}

} // namespace dcp::net
