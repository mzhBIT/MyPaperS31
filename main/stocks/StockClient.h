#pragma once
#include <string>
#include <vector>

namespace stocks {
struct Quote {
    std::string symbol, name, time;
    double last = 0, previous = 0, open = 0, high = 0, low = 0;
    bool valid = false;
};
bool normalize_symbol(const std::string &input, std::string &symbol);
std::vector<Quote> parse_quotes(const std::string &body, const std::vector<std::string> &symbols);
bool fetch(const std::vector<std::string> &symbols, std::vector<Quote> &quotes, std::string &error);
std::vector<std::string> load_watchlist();
bool save_watchlist(const std::vector<std::string> &symbols);
}
