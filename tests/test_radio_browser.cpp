#include "services/radio_browser_client.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

namespace {
class CurlFixture {
public:
    CurlFixture() {
        char directory[] = "/tmp/lilygo-radio-browser-test-XXXXXX";
        const char *created = mkdtemp(directory);
        assert(created);
        directory_ = created;
        if (const char *path = std::getenv("PATH")) old_path_ = path;
        write("curl", "#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"$RADIO_TEST_ARGS\"\nif [ \"$RADIO_TEST_SLOW\" = 1 ]; then exec /bin/sleep 5; fi\nif [ \"$RADIO_TEST_FAIL\" = 1 ]; then exit 22; fi\nif [ \"$RADIO_TEST_MIRROR\" = 1 ]; then\n  case \"$*\" in *all.api.radio-browser.info*|*de1.api.radio-browser.info*) exit 22;; esac\nfi\nexec /bin/cat \"$RADIO_TEST_PAYLOAD\"\n");
        assert(chmod((directory_ + "/curl").c_str(), 0700) == 0);
        assert(setenv("PATH", directory_.c_str(), 1) == 0);
        assert(setenv("RADIO_TEST_PAYLOAD", (directory_ + "/payload").c_str(), 1) == 0);
        assert(setenv("RADIO_TEST_ARGS", (directory_ + "/args").c_str(), 1) == 0);
    }
    ~CurlFixture() {
        (void)setenv("PATH", old_path_.c_str(), 1);
        (void)unsetenv("RADIO_TEST_PAYLOAD"); (void)unsetenv("RADIO_TEST_ARGS");
        (void)unsetenv("RADIO_TEST_SLOW"); (void)unsetenv("RADIO_TEST_FAIL");
        (void)unsetenv("RADIO_TEST_MIRROR");
        std::filesystem::remove_all(directory_);
    }
    void payload(const std::string &value) { write("payload", value); }
    std::string arguments() const {
        std::ifstream stream(directory_ + "/args");
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
private:
    void write(const std::string &name, const std::string &value) {
        std::ofstream stream(directory_ + "/" + name); stream << value; assert(stream.good());
    }
    std::string directory_;
    std::string old_path_;
};
RadioBrowserResult await_result(RadioBrowserClient &client) {
    RadioBrowserResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!client.poll(result)) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(!client.loading());
    RadioBrowserResult unused;
    assert(!client.poll(unused));
    return result;
}
}

int main(int argc, char **argv)
{
    CurlFixture fixture;
    RadioBrowserClient client;
    fixture.payload(R"json([
        {"name" : "\u4e2d\u6587 {Radio} \"Mix\" \ud83c\udfb5", "url_resolved": "https://radio.example/live", "url":"https://fallback.example/live", "country":"China", "countrycode":"CN", "codec":"MP3", "bitrate":128, "stationuuid":"station-1", "tags":"jazz,pop", "language":"chinese", "favicon":"https://radio.example/icon.png", "other":{"nested":[true,false,null,1.5e2,{"a":"}"}]}},
        {"name":"Fallback", "url_resolved":"ftp://invalid.example/live", "url":"https://valid.example/live", "bitrate":null},
        {"name":"Next page", "url":"https://next.example/live"}
    ])json");
    assert(client.search({"a&b +\";中文", 7, 2}));
    assert(client.loading());
    auto result = await_result(client);
    assert(result.error.empty() && result.stations.size() == 2 && result.has_more);
    assert(result.stations[0].name == "中文 {Radio} \"Mix\" 🎵");
    assert(result.stations[0].url == "https://radio.example/live");
    assert(result.stations[0].uuid == "station-1" && result.stations[0].country_code == "CN");
    assert(result.stations[0].tags == "jazz,pop" && result.stations[0].language == "chinese");
    assert(result.stations[0].bitrate == 128);
    assert(result.stations[1].url == "https://valid.example/live" && result.stations[1].bitrate == 0);
    assert(fixture.arguments().find("&offset=7&") != std::string::npos);
    assert(fixture.arguments().find("?limit=3&") != std::string::npos);
    assert(fixture.arguments().find("&name=a%26b%20%2B%22%3B%E4%B8%AD%E6%96%87") != std::string::npos);

    fixture.payload("[]");
    assert(client.search({"unfindable", 0, 50}));
    result = await_result(client);
    assert(result.error.empty() && result.stations.empty() && !result.has_more);

    for (const std::string payload : {"not json", "[{\"name\":\"x\",\"url\":\"https://x.test\"}", "[{\"name\":\"\\ud800\",\"url\":\"https://x.test\"}]", "[] trailing", "[{\"name\":\"x\",}]", "[{\"name\":42}]", "[{\"name\":\"x\",\"url\":true}]", "[{\"name\":\"x\",\"bitrate\":\"128\"}]"}) {
        fixture.payload(payload);
        assert(client.search({}));
        result = await_result(client);
        assert(!result.error.empty() && result.stations.empty());
    }

    fixture.payload(R"([{"name":"Invalid","url":"file:///etc/passwd"},{"name":"Second","url":"https://second.example/live"}])");
    assert(client.search({"", 0, 1}));
    result = await_result(client);
    assert(result.error.empty() && result.stations.empty() && result.has_more);

    fixture.payload(R"([{"name":"Broken","url":"https://broken.example/live","lastcheckok":0},{"name":"Healthy","url":"https://healthy.example/live","lastcheckok":1}])");
    assert(client.search({}));
    result = await_result(client);
    assert(result.error.empty() && result.stations.size() == 1 && result.stations[0].name == "Healthy");

    std::string many = "[";
    for (int i = 0; i < 120; ++i) {
        if (i != 0) many += ',';
        many += R"({"name":"Radio","url":"https://radio.example/live","lastcheckok":1})";
    }
    fixture.payload(many + ']');
    assert(client.search({"", 0, 1000}));
    result = await_result(client);
    assert(result.error.empty() && result.stations.size() == 100 && result.has_more);

    fixture.payload("[]");
    assert(setenv("RADIO_TEST_MIRROR", "1", 1) == 0);
    assert(client.search({"fallback", 0, 50}));
    result = await_result(client);
    assert(result.error.empty());
    assert(fixture.arguments().find("https://de1.api.radio-browser.info/json/stations/search?limit=51&offset=0&hidebroken=true&order=clickcount&reverse=true&name=fallback") != std::string::npos);
    assert(fixture.arguments().find("https://de2.api.radio-browser.info/json/stations/search?limit=51&offset=0&hidebroken=true&order=clickcount&reverse=true&name=fallback") != std::string::npos);
    (void)unsetenv("RADIO_TEST_MIRROR");

    assert(setenv("RADIO_TEST_FAIL", "1", 1) == 0);
    assert(client.search({}));
    result = await_result(client);
    assert(!result.error.empty());
    (void)unsetenv("RADIO_TEST_FAIL");

    assert(setenv("RADIO_TEST_SLOW", "1", 1) == 0);
    assert(client.search({"slow", 0, 10}));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    (void)unsetenv("RADIO_TEST_SLOW");
    fixture.payload("[]");
    const auto before = std::chrono::steady_clock::now();
    assert(client.search({"replacement", 0, 10}));
    assert(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(200));
    result = await_result(client);
    assert(result.error.empty() && result.stations.empty());

    fixture.payload(std::string(2U * 1024U * 1024U + 1U, ' '));
    assert(client.search({}));
    result = await_result(client);
    assert(!result.error.empty());

    if (argc == 2) {
        std::ifstream stream(argv[1]);
        assert(stream.good());
        fixture.payload({std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()});
        assert(client.search({"", 0, 100}));
        result = await_result(client);
        assert(result.error.empty() && !result.stations.empty());
        std::cout << "Live payload parsed: " << result.stations.size() << " stations\n";
    }
    std::cout << "Radio Browser tests passed\n";
}
