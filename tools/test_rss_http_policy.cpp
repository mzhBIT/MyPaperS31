#include "rss/RssHttpPolicy.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace rss_http;
    std::string out, origin, host;
    assert(split_url("https://www.ithome.com/rss", origin, host));
    assert(origin == "https://www.ithome.com" && host == "www.ithome.com");
    assert(split_url("http://example.org:8080/a", origin, host));
    assert(host == "example.org" && origin == "http://example.org:8080");
    assert(redirect_target("http://www.ithome.com/rss", "https://www.ithome.com/rss", out));
    assert(out == "https://www.ithome.com/rss");
    assert(redirect_target(out, "/new/feed.xml?x=1", out));
    assert(out == "https://www.ithome.com/new/feed.xml?x=1");
    assert(redirect_target("https://example.org/a/b/rss", "../feed.xml", out));
    assert(out == "https://example.org/a/feed.xml");
    assert(redirect_target("https://example.org/a/rss", "?page=2", out));
    assert(out == "https://example.org/a/rss?page=2");
    assert(redirect_target("https://example.org/a", "//other.org/rss", out));
    assert(out == "https://other.org/rss");
    assert(redirect_target("https://example.org/a?b=2", "#part", out));
    assert(out == "https://example.org/a?b=2");
    assert(!redirect_target("https://example.org/rss", "http://example.org/rss", out));
    assert(!redirect_target("http://example.org/rss", "file:///etc/passwd", out));
    assert(!redirect_target("http://example.org/rss", "ftp://example.org/rss", out));
    assert(!redirect_target("http://example.org/rss", "https://user:secret@example.org/rss", out));
    assert(!redirect_target("http://example.org/rss", "/x\r\nInjected: header", out));
    assert(!redirect_target("http://example.org/rss", "", out));
    for (int status : {301,302,303,307,308}) assert(redirect_status(status));
    for (int status : {200,304,400,500}) assert(!redirect_status(status));
    assert(max_redirects == 3);
    std::puts("RSS HTTP policy: redirects, relative URLs, upgrade/downgrade and invalid targets PASS");
}
