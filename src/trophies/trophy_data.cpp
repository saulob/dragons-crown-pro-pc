#include "trophies/trophy_data.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace dcp::trophies {
namespace {

constexpr size_t kMaxXmlBytes = 4u << 20;
constexpr int kMaxDepth = 16;
constexpr int kMaxNodes = 8192;

struct Node {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;
    std::vector<Node> children;

    const std::string* Attribute(std::string_view key) const {
        for (const auto& [name_, value] : attributes) {
            if (name_ == key) {
                return &value;
            }
        }
        return nullptr;
    }
    const Node* Child(std::string_view child_name) const {
        for (const Node& child : children) {
            if (child.name == child_name) {
                return &child;
            }
        }
        return nullptr;
    }
};

void AppendUtf8(std::string& out, unsigned long code) {
    if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return;
    }
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | code >> 6);
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | code >> 12);
        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | code >> 18);
        out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

std::string DecodeEntities(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') {
            out += raw[i];
            continue;
        }
        const size_t end = raw.find(';', i);
        if (end == std::string_view::npos || end - i > 12) {
            out += raw[i];
            continue;
        }
        const std::string_view entity = raw.substr(i + 1, end - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else if (entity.size() > 1 && entity[0] == '#') {
            const bool hex = entity[1] == 'x' || entity[1] == 'X';
            const std::string digits(entity.substr(hex ? 2 : 1));
            char* stop = nullptr;
            const unsigned long code = digits.empty() ? 0 : std::strtoul(digits.c_str(), &stop, hex ? 16 : 10);
            if (digits.empty() || *stop != '\0') {
                out.append(raw.substr(i, end - i + 1));
            } else {
                AppendUtf8(out, code);
            }
        } else {
            out.append(raw.substr(i, end - i + 1));
        }
        i = end;
    }
    return out;
}

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string Trim(std::string_view text) {
    while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
    return std::string(text);
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool Parse(Node& root) {
        if (s_.substr(0, 3) == "\xEF\xBB\xBF") {
            p_ = 3;
        }
        while (SkipMisc()) {
        }
        if (p_ >= s_.size() || s_[p_] != '<') {
            return false;
        }
        if (!ParseElement(root, 0)) {
            return false;
        }
        while (SkipMisc()) {
        }
        return p_ == s_.size();
    }

private:
    bool Starts(std::string_view prefix) const { return s_.substr(p_, prefix.size()) == prefix; }

    bool SkipPast(std::string_view terminator) {
        const size_t at = s_.find(terminator, p_);
        if (at == std::string_view::npos) {
            return false;
        }
        p_ = at + terminator.size();
        return true;
    }

    bool SkipMisc() {
        const size_t start = p_;
        while (p_ < s_.size() && IsSpace(s_[p_])) ++p_;
        if (Starts("<?")) {
            if (!SkipPast("?>")) p_ = s_.size();
        } else if (Starts("<!--")) {
            if (!SkipPast("-->")) p_ = s_.size();
        } else if (Starts("<!")) {
            if (!SkipPast(">")) p_ = s_.size();
        }
        return p_ != start;
    }

    static bool NameChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == ':' || c == '.';
    }

    bool ParseName(std::string& name) {
        const size_t start = p_;
        while (p_ < s_.size() && NameChar(s_[p_])) ++p_;
        name.assign(s_.substr(start, p_ - start));
        return !name.empty();
    }

    bool ParseElement(Node& node, int depth) {
        if (depth > kMaxDepth || ++nodes_ > kMaxNodes || !Starts("<")) {
            return false;
        }
        ++p_;
        if (!ParseName(node.name)) {
            return false;
        }
        for (;;) {
            while (p_ < s_.size() && IsSpace(s_[p_])) ++p_;
            if (p_ >= s_.size()) {
                return false;
            }
            if (Starts("/>")) {
                p_ += 2;
                return true;
            }
            if (s_[p_] == '>') {
                ++p_;
                break;
            }
            std::string key;
            if (!ParseName(key)) {
                return false;
            }
            while (p_ < s_.size() && IsSpace(s_[p_])) ++p_;
            if (p_ >= s_.size() || s_[p_] != '=') {
                return false;
            }
            ++p_;
            while (p_ < s_.size() && IsSpace(s_[p_])) ++p_;
            if (p_ >= s_.size() || (s_[p_] != '"' && s_[p_] != '\'')) {
                return false;
            }
            const char quote = s_[p_++];
            const size_t end = s_.find(quote, p_);
            if (end == std::string_view::npos) {
                return false;
            }
            node.attributes.emplace_back(std::move(key), DecodeEntities(s_.substr(p_, end - p_)));
            p_ = end + 1;
        }
        std::string text;
        for (;;) {
            const size_t lt = s_.find('<', p_);
            if (lt == std::string_view::npos) {
                return false;
            }
            text += DecodeEntities(s_.substr(p_, lt - p_));
            p_ = lt;
            if (Starts("</")) {
                p_ += 2;
                std::string closing;
                if (!ParseName(closing) || closing != node.name) {
                    return false;
                }
                while (p_ < s_.size() && IsSpace(s_[p_])) ++p_;
                if (p_ >= s_.size() || s_[p_] != '>') {
                    return false;
                }
                ++p_;
                node.text = node.children.empty() ? Trim(text) : std::string();
                return true;
            }
            if (Starts("<![CDATA[")) {
                p_ += 9;
                const size_t end = s_.find("]]>", p_);
                if (end == std::string_view::npos) {
                    return false;
                }
                text.append(s_.substr(p_, end - p_));
                p_ = end + 3;
            } else if (Starts("<!--")) {
                if (!SkipPast("-->")) {
                    return false;
                }
            } else if (Starts("<?")) {
                if (!SkipPast("?>")) {
                    return false;
                }
            } else {
                node.children.emplace_back();
                if (!ParseElement(node.children.back(), depth + 1)) {
                    return false;
                }
            }
        }
    }

    std::string_view s_;
    size_t p_ = 0;
    int nodes_ = 0;
};

std::string ChildText(const Node& node, std::string_view name) {
    const Node* child = node.Child(name);
    return child ? child->text : std::string();
}

bool ParseId(const std::string* text, int& id) {
    if (!text || text->empty() || text->size() > 6) {
        return false;
    }
    const bool negative = (*text)[0] == '-';
    if (negative && text->size() == 1) {
        return false;
    }
    int value = 0;
    for (size_t i = negative ? 1 : 0; i < text->size(); ++i) {
        const char c = (*text)[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    id = negative ? -value : value;
    return true;
}

bool IsTrue(const std::string* text) {
    return text && !text->empty() && std::strchr("tTyY1", (*text)[0]) != nullptr;
}

Grade ParseGrade(const std::string* text) {
    if (!text || text->size() != 1) {
        return Grade::kUnknown;
    }
    switch ((*text)[0]) {
    case 'P': return Grade::kPlatinum;
    case 'G': return Grade::kGold;
    case 'S': return Grade::kSilver;
    case 'B': return Grade::kBronze;
    }
    return Grade::kUnknown;
}

} // namespace

const char* GradeName(Grade grade) {
    switch (grade) {
    case Grade::kPlatinum: return "Platinum";
    case Grade::kGold: return "Gold";
    case Grade::kSilver: return "Silver";
    case Grade::kBronze: return "Bronze";
    case Grade::kUnknown: break;
    }
    return "Unknown";
}

const Trophy* TrophySet::Find(int id) const {
    const auto it = std::lower_bound(trophies.begin(), trophies.end(), id,
                                     [](const Trophy& trophy, int wanted) { return trophy.id < wanted; });
    return it != trophies.end() && it->id == id ? &*it : nullptr;
}

int TrophySet::Count(Grade grade) const {
    return static_cast<int>(std::count_if(trophies.begin(), trophies.end(),
                                          [grade](const Trophy& trophy) { return trophy.grade == grade; }));
}

std::string TrophyIconName(int id) {
    char name[24];
    std::snprintf(name, sizeof(name), "TROP%03d.PNG", id);
    return name;
}

std::optional<TrophySet> ParseTrophyConf(std::string_view xml) {
    if (xml.empty() || xml.size() > kMaxXmlBytes) {
        return std::nullopt;
    }
    Node root;
    if (!Parser(xml).Parse(root) || root.name != "trophyconf") {
        return std::nullopt;
    }
    TrophySet set;
    set.np_comm_id = ChildText(root, "npcommid");
    for (const Node& child : root.children) {
        if (child.name != "trophy") {
            continue;
        }
        Trophy trophy;
        trophy.grade = ParseGrade(child.Attribute("ttype"));
        if (!ParseId(child.Attribute("id"), trophy.id) || trophy.id < 0 || trophy.id >= kMaxTrophies || trophy.grade == Grade::kUnknown) {
            return std::nullopt;
        }
        trophy.hidden = IsTrue(child.Attribute("hidden"));
        ParseId(child.Attribute("pid"), trophy.platinum_link);
        ParseId(child.Attribute("gid"), trophy.group);
        set.trophies.push_back(std::move(trophy));
    }
    if (set.trophies.empty() || set.trophies.size() > static_cast<size_t>(kMaxTrophies)) {
        return std::nullopt;
    }
    std::sort(set.trophies.begin(), set.trophies.end(), [](const Trophy& a, const Trophy& b) { return a.id < b.id; });
    for (size_t i = 1; i < set.trophies.size(); ++i) {
        if (set.trophies[i].id == set.trophies[i - 1].id) {
            return std::nullopt;
        }
    }
    return set;
}

std::optional<TrophyTexts> ParseTrophyTexts(std::string_view xml) {
    if (xml.empty() || xml.size() > kMaxXmlBytes) {
        return std::nullopt;
    }
    Node root;
    if (!Parser(xml).Parse(root) || root.name != "trophyconf") {
        return std::nullopt;
    }
    TrophyTexts texts;
    texts.title = ChildText(root, "title-name");
    texts.detail = ChildText(root, "title-detail");
    for (const Node& child : root.children) {
        if (child.name == "group") {
            ++texts.groups;
        } else if (child.name == "trophy") {
            Trophy trophy;
            if (ParseId(child.Attribute("id"), trophy.id)) {
                trophy.name = ChildText(child, "name");
                trophy.detail = ChildText(child, "detail");
                texts.trophies.push_back(std::move(trophy));
            }
        }
    }
    return texts;
}

void ApplyTrophyTexts(TrophySet& set, const TrophyTexts& texts) {
    set.title = texts.title;
    set.detail = texts.detail;
    set.groups = texts.groups;
    for (const Trophy& text : texts.trophies) {
        const auto it = std::lower_bound(set.trophies.begin(), set.trophies.end(), text.id,
                                         [](const Trophy& trophy, int wanted) { return trophy.id < wanted; });
        if (it != set.trophies.end() && it->id == text.id) {
            it->name = text.name;
            it->detail = text.detail;
        }
    }
}

bool CountsTowardPlatinum(const Trophy& trophy) {
    return trophy.grade != Grade::kPlatinum && trophy.platinum_link != kNoTrophy;
}

} // namespace dcp::trophies
