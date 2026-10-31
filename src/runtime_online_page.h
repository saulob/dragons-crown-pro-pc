// The Online page's data, copied from the launcher's models into the overlay's plain structs.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "net/connection_test.h"
#include "online_view.h"
#include "runtime_online_overlay.h"

namespace dcp::online {

// Copies text that fits and cuts it on a character boundary when it does not.
template <size_t N>
void CopyText(char (&out)[N], const std::string& text) {
    size_t length = std::min(text.size(), N - 1);
    while (length > 0 && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    std::memcpy(out, text.data(), length);
    out[length] = '\0';
}

inline RuntimeOnlineFriend FriendRowOf(const OnlinePlayer& player) {
    const FriendStatus status = FriendStatusOf(player);
    RuntimeOnlineFriend row{};
    CopyText(row.name, player.name);
    CopyText(row.status, status.text);
    row.online = player.online ? 1 : 0;
    row.incompatible = status.incompatible ? 1 : 0;
    return row;
}

inline void FillReport(RuntimeOnlinePage& page, const net::ConnectionReport& report) {
    using Mark = net::ConnectionReport::Mark;
    page.test_ready = report.ready ? 1 : 0;
    CopyText(page.test_verdict, report.verdict);
    page.test_line_count = 0;
    for (const net::ConnectionReport::Line& line : report.lines) {
        if (page.test_line_count >= RUNTIME_ONLINE_MAX_TEST_LINES) {
            break;
        }
        RuntimeOnlineTestLine& out = page.test_lines[page.test_line_count++];
        out.mark = line.mark == Mark::kOk     ? RUNTIME_ONLINE_MARK_OK
                   : line.mark == Mark::kNote ? RUNTIME_ONLINE_MARK_NOTE
                                              : RUNTIME_ONLINE_MARK_PROBLEM;
        CopyText(out.label, line.label);
        CopyText(out.text, line.text);
    }
}

inline void FillFriends(RuntimeOnlinePage& page, const std::vector<OnlinePlayer>& friends) {
    page.friend_total = int(friends.size());
    page.friend_count = 0;
    for (const OnlinePlayer& player : friends) {
        if (page.friend_count >= RUNTIME_ONLINE_MAX_FRIENDS) {
            break;
        }
        page.friends[page.friend_count++] = FriendRowOf(player);
    }
}

} // namespace dcp::online
