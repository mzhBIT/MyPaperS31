#include "map/AmapClient.h"
#include <cassert>
#include <cstdio>
int main()
{
    auto r = AmapClient::parse_search_response(R"({"status":"1","pois":[{"name":"West Lake","address":"road","location":"120.1,30.2"},{"name":"bad","location":"nan,30"},{"name":"bad","location":"181,30"},{"name":"bad","location":[]}]})");
    assert(r.success && r.places.size() == 1 && r.places[0].name == "West Lake");
    assert(r.places[0].longitude == 120.1 && r.places[0].address == "road");
    r = AmapClient::parse_search_response(R"({"status":"1","pois":[]})");
    assert(r.success && r.places.empty());
    r = AmapClient::parse_search_response(R"({"status":"1","pois":{}})");
    assert(!r.success);
    r = AmapClient::parse_search_response(R"({"status":"0","infocode":"10021"})");
    assert(!r.success && r.error == "AMap request rate limit reached");
    assert(!AmapClient::parse_search_response("truncated").success);
    r = AmapClient::parse_search_response(R"({"status":"1","pois":[{"name":"\u897f\u6e56","location":"120,30"}]})");
    assert(r.success && r.places.size() == 1 && r.places[0].name.size() == 6);
    r = AmapClient::parse_location_response(R"({"status":"1","province":"Zhejiang","city":"Hangzhou","rectangle":"119,29;121,31"})",true,0,0);
    assert(r.success && r.places.size()==1 && r.places[0].longitude==120 && r.places[0].latitude==30);
    assert(!AmapClient::parse_location_response(R"({"status":"1","rectangle":[],"city":[]})",true,0,0).success);
    assert(!AmapClient::parse_location_response(R"({"status":"1","rectangle":"121,31;119,29"})",true,0,0).success);
    r = AmapClient::parse_location_response(R"({"status":"1","regeocode":{"formatted_address":"Hangzhou road","addressComponent":{"district":"Xihu","township":"Town"}}})",false,120,30);
    assert(r.success && r.places[0].address=="Hangzhou road" && r.places[0].name=="XihuTown" && r.places[0].longitude==120);
    assert(!AmapClient::parse_location_response(R"({"status":"1","regeocode":{"formatted_address":[]}})",false,120,30).success);
    assert(!AmapClient::parse_location_response(R"({"status":"0","infocode":"10001"})",true,0,0).success);
    std::puts("AMap search parser passed: results, empty, errors, invalid coordinates and Unicode");
}
