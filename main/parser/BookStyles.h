#pragma once

#include "renderer/InlineStyle.h"
#include "renderer/ParagraphAlignment.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace book_styles {

using View = std::string_view;
using namespace std::literals::string_view_literals;

inline View trim(View value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

inline float compute(const std::string &value, float parent, float root)
{
    if (value == "inherit" || value == "unset") return parent;
    if (value == "smaller") return parent / 1.2f;
    if (value == "larger") return parent * 1.2f;
    constexpr View keywords[] = {"xx-small", "x-small", "small", "medium",
                                 "large", "x-large", "xx-large", "xxx-large"};
    constexpr float sizes[] = {9, 10, 13, 16, 18, 24, 32, 48};
    if (value == "initial") return 16;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        if (value == keywords[i]) return sizes[i];
    }
    char *end = nullptr;
    const float number = std::strtof(value.c_str(), &end);
    if (end == value.c_str() || !std::isfinite(number) || number <= 0) return 0;
    const View unit(end);
    if (unit == "%"sv) return number * parent / 100;
    if (unit == "em"sv) return number * parent;
    if (unit == "rem"sv) return number * root;
    if (unit == "px"sv) return number;
    if (unit == "pt"sv) return number * (4.f / 3);
    if (unit == "pc"sv) return number * 16;
    return 0;
}

struct Declaration {
    std::string value;
    bool important = false;
};

struct Declarations {
    Declaration font_size;
    Declaration alignment;
};

inline ParagraphAlignment alignment_value(View value)
{
    if (value == "left" || value == "start" || value == "initial") return ParagraphAlignment::Start;
    if (value == "center") return ParagraphAlignment::Center;
    if (value == "right" || value == "end") return ParagraphAlignment::End;
    if (value == "justify") return ParagraphAlignment::Justify;
    return ParagraphAlignment::Automatic;
}

inline Declarations declarations(View css)
{
    Declarations result;
    while (!css.empty()) {
        const auto end = css.find(';');
        const View entry = trim(css.substr(0, end));
        css = end == View::npos ? View() : css.substr(end + 1);
        const auto colon = entry.find(':');
        if (colon == View::npos) continue;
        std::string property(trim(entry.substr(0, colon)));
        for (char &ch : property) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (property != "font-size" && property != "text-align") continue;
        std::string value(trim(entry.substr(colon + 1)));
        for (char &ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const auto bang = value.find('!');
        const bool important = bang != std::string::npos && trim(View(value).substr(bang + 1)) == "important";
        if (bang != std::string::npos) {
            if (!important) continue;
            value = std::string(trim(View(value).substr(0, bang)));
        }
        if (property == "font-size") {
            if ((!result.font_size.important || important) && compute(value, 16, 16) > 0)
                result.font_size = {std::move(value), important};
        } else if ((!result.alignment.important || important) &&
                   (alignment_value(value) != ParagraphAlignment::Automatic ||
                    value == "inherit" || value == "unset" || value == "match-parent")) {
            result.alignment = {std::move(value), important};
        }
    }
    return result;
}

struct Element {
    std::string name, id, classes, original_name;
    float size = 16;
    ParagraphAlignment alignment = ParagraphAlignment::Automatic;
    bool block = false;
};

struct SelectorPart {
    std::string text;
    bool direct_parent = false;
};

struct Rule {
    std::vector<SelectorPart> selector;
    Declarations style;
    unsigned specificity = 0;
};

inline bool name_char(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' ||
           static_cast<unsigned char>(ch) >= 128;
}

inline bool class_matches(View classes, View target)
{
    while (!(classes = trim(classes)).empty()) {
        const auto end = classes.find_first_of(" \t\r\n\f");
        if (classes.substr(0, end) == target) return true;
        if (end == View::npos) break;
        classes.remove_prefix(end + 1);
    }
    return false;
}

inline bool matches(View part, const Element &element)
{
    const auto end = part.find_first_of("#.");
    const View name = part.substr(0, end);
    if (!name.empty() && name != "*" && name != element.name && name != element.original_name) return false;
    if (end == View::npos) return true;
    part.remove_prefix(end);
    while (!part.empty()) {
        const bool id = part.front() == '#';
        part.remove_prefix(1);
        const auto next = part.find_first_of("#.");
        const View value = part.substr(0, next);
        if (id ? value != element.id : !class_matches(element.classes, value)) return false;
        if (next == View::npos) break;
        part.remove_prefix(next);
    }
    return true;
}

inline bool matches(const Rule &rule, const std::vector<Element> &parents,
                    const Element &element)
{
    if (!matches(rule.selector.back().text, element)) return false;
    size_t ancestor = parents.size();
    for (size_t i = rule.selector.size() - 1; i > 0; --i) {
        if (!ancestor) return false;
        const bool direct = rule.selector[i].direct_parent;
        do {
            if (matches(rule.selector[i - 1].text, parents[--ancestor])) break;
            if (direct || ancestor == 0) return false;
        } while (true);
    }
    return true;
}

inline void add_rule(View selector, const Declarations &style, std::vector<Rule> *rules)
{
    Rule rule;
    bool direct = false;
    std::string normalized;
    const auto attr = selector.find("[name=");
    if (attr != View::npos) {
        if (attr < 5 || selector.substr(attr - 5, 5) != "style") return;
        const auto end = selector.find(']', attr + 6);
        if (end == View::npos) return;
        View name = trim(selector.substr(attr + 6, end - attr - 6));
        if (name.size() >= 2 && (name.front() == '\'' || name.front() == '"') && name.back() == name.front())
            name = name.substr(1, name.size() - 2);
        if (name.empty() || !std::all_of(name.begin(), name.end(), name_char)) return;
        normalized = std::string(selector.substr(0, attr)) + "." + std::string(name) +
                     std::string(selector.substr(end + 1));
        selector = normalized;
    }
    while (!(selector = trim(selector)).empty()) {
        if (selector.front() == '>') {
            if (direct || rule.selector.empty()) return;
            direct = true;
            selector.remove_prefix(1);
            continue;
        }
        const auto end = selector.find_first_of(" >\t\r\n\f");
        const View part = selector.substr(0, end);
        if (rule.selector.size() == 8) return;
        if (part == ":root") {
            rule.selector.push_back({"html", direct});
            rule.specificity += 256;
            direct = false;
            if (end == View::npos) break;
            selector.remove_prefix(end);
            continue;
        }
        size_t i = part.front() == '*' ? 1 : 0;
        while (i < part.size() && name_char(part[i])) ++i;
        if (i && part.front() != '*') ++rule.specificity;
        while (i < part.size()) {
            if (part[i] != '.' && part[i] != '#') return;
            rule.specificity += part[i++] == '#' ? 65536 : 256;
            const size_t begin = i;
            while (i < part.size() && name_char(part[i])) ++i;
            if (i == begin) return;
        }
        rule.selector.push_back({std::string(part), direct});
        direct = false;
        if (end == View::npos) break;
        selector.remove_prefix(end);
    }
    if (rule.selector.empty() || direct) return;
    rule.style = style;
    rules->push_back(std::move(rule));
}

inline void add_stylesheet(std::string css, std::vector<Rule> *rules)
{
    size_t src = 0;
    size_t dst = 0;
    size_t comment = 0;
    while ((comment = css.find("/*", src)) != std::string::npos) {
        for (; src < comment; ++src) css[dst++] = css[src];
        const size_t end = css.find("*/", comment + 2);
        css[dst++] = ' ';
        src = end == std::string::npos ? css.size() : end + 2;
    }
    if (dst != src) {
        for (; src < css.size(); ++src) css[dst++] = css[src];
        css.resize(dst);
    }
    View rest(css);
    rest = trim(rest);
    if (rest.size() >= 12 && rest.substr(0, 9) == "<![CDATA[" && rest.substr(rest.size() - 3) == "]]>")
        rest = rest.substr(9, rest.size() - 12);
    while (!(rest = trim(rest)).empty() && rules->size() < 512) {
        const auto open = rest.find('{');
        if (open == View::npos) break;
        const auto semi = rest.find(';');
        if (semi < open) { rest.remove_prefix(semi + 1); continue; }
        size_t close = open + 1;
        unsigned depth = 1;
        for (; close < rest.size(); ++close) {
            if (rest[close] == '{') ++depth;
            if (rest[close] == '}' && --depth == 0) break;
        }
        if (close == rest.size()) break;
        if (rest.front() != '@') {
            const auto style = declarations(rest.substr(open + 1, close - open - 1));
            if (!style.font_size.value.empty() || !style.alignment.value.empty()) {
                View selectors = rest.substr(0, open);
                while (!selectors.empty() && rules->size() < 512) {
                    const auto comma = selectors.find(',');
                    add_rule(selectors.substr(0, comma), style, rules);
                    if (comma == View::npos) break;
                    selectors.remove_prefix(comma + 1);
                }
            }
        }
        rest.remove_prefix(close + 1);
    }
}

inline Declarations element_style(const Element &element, const std::vector<Element> &parents,
                                   const std::vector<Rule> &rules, View inline_css)
{
    const Rule *best_font = nullptr;
    const Rule *best_alignment = nullptr;
    auto wins = [](const Rule &rule, const Rule *best, Declaration Declarations::*member) {
        const auto &candidate = rule.style.*member;
        if (candidate.value.empty()) return false;
        if (!best) return true;
        const auto &previous = best->style.*member;
        return candidate.important != previous.important
            ? candidate.important : rule.specificity >= best->specificity;
    };
    for (const auto &rule : rules) {
        const bool font_wins = wins(rule, best_font, &Declarations::font_size);
        const bool alignment_wins = wins(rule, best_alignment, &Declarations::alignment);
        if ((!font_wins && !alignment_wins) || !matches(rule, parents, element)) continue;
        if (font_wins) best_font = &rule;
        if (alignment_wins) best_alignment = &rule;
    }
    auto style = declarations(inline_css);
    auto apply_rule = [](const Rule *best, Declaration Declarations::*member, Declaration &local) {
        if (!best) return;
        const auto &rule_value = best->style.*member;
        if (local.value.empty() || (rule_value.important && !local.important)) local = rule_value;
    };
    apply_rule(best_font, &Declarations::font_size, style.font_size);
    apply_rule(best_alignment, &Declarations::alignment, style.alignment);
    return style;
}

inline float element_size(const Element &element, const Declaration &font, View legacy_size,
                          float parent, float root)
{
    const float specified = compute(font.value, parent, root);
    if (specified > 0) return std::clamp(specified, 1.f, 1024.f);
    const View name(element.name);
    if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6')
        return parent * inline_style::heading_scale(name[1] - '0') / 100.f;
    if (name == "small") return parent / 1.2f;
    if (name == "big") return parent * 1.2f;
    if (name == "sup" || name == "sub") return parent * .65f;
    if (name == "font" && !legacy_size.empty()) {
        const std::string raw(legacy_size);
        char *end = nullptr;
        const long value = std::strtol(raw.c_str(), &end, 10);
        if (end != raw.c_str() && *end == '\0') {
            constexpr float sizes[] = {10, 13, 16, 18, 24, 32, 48};
            const long size = (raw.front() == '+' || raw.front() == '-') ? 3 + std::clamp(value, -7L, 7L) : value;
            return sizes[std::clamp(size, 1L, 7L) - 1];
        }
    }
    return parent;
}

inline uint16_t ratio(float size, float base)
{
    return static_cast<uint16_t>(std::lround(std::clamp(size / base * 100.f, 25.f, 300.f)));
}

}  // namespace book_styles
