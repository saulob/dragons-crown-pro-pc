// SPDX-License-Identifier: GPL-2.0-or-later
// The Options menu's Online page: the rows and the note for a copy of the Online state. Read only except for copying the
// friend code and running the Connection test.
#pragma once

#include <string>
#include <vector>

#include "dcp_options_menu.h"
#include "runtime_online_overlay.h"

namespace DcpOptions {

enum class OnlineAction { None, CopyCode, RunTest };

struct OnlineRow {
    std::string label;
    std::string value;
    std::string help;
    bool lit = false;
    bool warn = false;
    OnlineAction action = OnlineAction::None;
};

inline const std::string kDetailSeparator = " \xC2\xB7 ";

inline std::string TestHelp(const RuntimeOnlinePage& page) {
    std::string help = "Checks that you can reach the online service and that other players can reach you. Takes a few "
                       "seconds.";
    if (page.test == RUNTIME_ONLINE_TEST_RUNNING) {
        return help;
    }
    help += " Press A or Enter to run it.";
    if (page.test != RUNTIME_ONLINE_TEST_DONE) {
        return help;
    }
    for (int i = 0; i < page.test_line_count; ++i) {
        const RuntimeOnlineTestLine& line = page.test_lines[i];
        const char* mark = line.mark == RUNTIME_ONLINE_MARK_OK ? "OK" : line.mark == RUNTIME_ONLINE_MARK_NOTE ? "--" : "!!";
        help += std::string("\n") + mark + " " + line.label + ": " + line.text;
    }
    return help;
}

inline std::vector<OnlineRow> OnlineRowsOf(const RuntimeOnlinePage& page) {
    std::vector<OnlineRow> rows;
    rows.push_back({"Online name", page.name, "What other players see. It is changed in the launcher."});
    if (!page.active) {
        return rows;
    }

    const bool has_code = page.friend_code[0] != '\0';
    OnlineRow code{"Your friend code", page.friend_code, "Share this code so another player can add you."};
    if (has_code) {
        code.help += " Press A or Enter to copy it.";
        code.action = OnlineAction::CopyCode;
    } else {
        code.value = page.link == RUNTIME_ONLINE_LINK_CONNECTING ? "Connecting..." : "Not available right now";
    }
    rows.push_back(std::move(code));

    const bool running = page.test == RUNTIME_ONLINE_TEST_RUNNING;
    const bool done = page.test == RUNTIME_ONLINE_TEST_DONE;
    OnlineRow test{"Connection test", running ? "Testing..." : done ? page.test_verdict : "Run test", TestHelp(page)};
    test.lit = done && page.test_ready;
    test.warn = done && !page.test_ready;
    test.action = running ? OnlineAction::None : OnlineAction::RunTest;
    rows.push_back(std::move(test));

    for (int i = 0; i < page.friend_count; ++i) {
        const RuntimeOnlineFriend& friend_row = page.friends[i];
        const std::string status = friend_row.status;
        const size_t cut = status.find(kDetailSeparator);
        OnlineRow row{friend_row.name, status.substr(0, cut),
                      cut == std::string::npos ? std::string() : status.substr(cut + kDetailSeparator.size())};
        if (friend_row.incompatible) {
            row.value += "  \xC2\xB7  Incompatible";
        }
        row.lit = friend_row.online != 0;
        row.warn = friend_row.incompatible != 0;
        rows.push_back(std::move(row));
    }
    return rows;
}

inline std::string OnlineNote(const RuntimeOnlinePage& page) {
    std::string status;
    if (!page.active) {
        status = "Online play is not active in this session; the log says why.";
    } else if (page.link == RUNTIME_ONLINE_LINK_UNREACHABLE) {
        status = "Cannot reach the online service right now. Run the Connection test for details.";
    } else if (page.link == RUNTIME_ONLINE_LINK_OFF) {
        status = "The game has not connected to the online service yet.";
    } else if (page.link != RUNTIME_ONLINE_LINK_UP) {
        status = "Connecting to the online service...";
    } else if (!page.friends_known) {
        status = "The friend list appears once the game has loaded it.";
    } else if (page.friend_total == 0) {
        status = "No friends yet.";
    } else if (page.friend_total > page.friend_count) {
        status = std::to_string(page.friend_total - page.friend_count) + " more friends are listed in the launcher.";
    }
    const std::string note = PageNote(Page::Online);
    return status.empty() ? note : status + "\n" + note;
}

} // namespace DcpOptions
