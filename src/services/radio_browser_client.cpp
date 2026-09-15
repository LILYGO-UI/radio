#include "services/radio_browser_client.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr std::array<const char *, 3> kEndpoints{{
    "https://all.api.radio-browser.info/json/stations/search",
    "https://de1.api.radio-browser.info/json/stations/search",
    "https://de2.api.radio-browser.info/json/stations/search",
}};
constexpr std::size_t kMaximumPayload = 2U * 1024U * 1024U;
constexpr std::size_t kReadBudget = 64U * 1024U;

// Small strict JSON reader: strings include escaped Unicode and surrogate pairs;
// irrelevant fields are skipped structurally, so punctuation in names is safe.
class StationJsonReader {
public:
    explicit StationJsonReader(std::string_view input) : input_(input) {}

    bool read(std::vector<RadioStation> &stations, std::size_t limit, bool &has_more)
    {
        std::size_t records = 0;
        if (!consume('[')) return false;
        if (consume(']')) return at_end();
        do {
            RadioStation station;
            int last_check_ok = 1;
            std::string fallback_url;
            if (!consume('{')) return false;
            if (!consume('}')) {
                do {
                    std::string key;
                    if (!string(key) || !consume(':')) return false;
                    if (key == "name") { if (!nullable_string(station.name)) return false; }
                    else if (key == "url_resolved") { if (!nullable_string(station.url)) return false; }
                    else if (key == "url") { if (!nullable_string(fallback_url)) return false; }
                    else if (key == "country") { if (!nullable_string(station.country)) return false; }
                    else if (key == "codec") { if (!nullable_string(station.codec)) return false; }
                    else if (key == "stationuuid") { if (!nullable_string(station.uuid)) return false; }
                    else if (key == "tags") { if (!nullable_string(station.tags)) return false; }
                    else if (key == "countrycode") { if (!nullable_string(station.country_code)) return false; }
                    else if (key == "language") { if (!nullable_string(station.language)) return false; }
                    else if (key == "favicon") { if (!nullable_string(station.favicon)) return false; }
                    else if (key == "bitrate") { if (!integer(station.bitrate)) return false; }
                    else if (key == "lastcheckok") { if (!integer(last_check_ok)) return false; }
                    else if (!skip_value(0)) return false;
                } while (consume(','));
                if (!consume('}')) return false;
            }
            if (RadioModel::validate_stream_url(station.url) != StreamUrlStatus::Valid) station.url = std::move(fallback_url);
            ++records;
            has_more = records > limit;
            if (records <= limit && last_check_ok == 1 && !station.name.empty() && RadioModel::validate_stream_url(station.url) == StreamUrlStatus::Valid) {
                // Catalog records are untrusted display data; avoid controls in labels.
                sanitize(station.name); sanitize(station.country); sanitize(station.codec);
                station.bitrate = std::max(0, station.bitrate);
                stations.push_back(std::move(station));
            }
        } while (consume(','));
        return consume(']') && at_end();
    }

private:
    void whitespace() { while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\n' || input_[pos_] == '\r' || input_[pos_] == '\t')) ++pos_; }
    bool at_end() { whitespace(); return pos_ == input_.size(); }
    bool consume(char c) { whitespace(); if (pos_ == input_.size() || input_[pos_] != c) return false; ++pos_; return true; }
    bool literal(std::string_view value) {
        whitespace();
        if (input_.substr(pos_, value.size()) != value) return false;
        pos_ += value.size(); return true;
    }
    static void sanitize(std::string &value) { for (char &c : value) if (static_cast<unsigned char>(c) < 0x20U || c == 0x7f) c = ' '; }
    static void utf8(std::string &out, unsigned code) {
        if (code < 0x80U) out.push_back(static_cast<char>(code));
        else if (code < 0x800U) { out.push_back(static_cast<char>(0xc0U | (code >> 6U))); out.push_back(static_cast<char>(0x80U | (code & 63U))); }
        else if (code < 0x10000U) { out.push_back(static_cast<char>(0xe0U | (code >> 12U))); out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 63U))); out.push_back(static_cast<char>(0x80U | (code & 63U))); }
        else { out.push_back(static_cast<char>(0xf0U | (code >> 18U))); out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 63U))); out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 63U))); out.push_back(static_cast<char>(0x80U | (code & 63U))); }
    }
    bool hex4(unsigned &code) {
        code = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= input_.size()) return false;
            const char c = input_[pos_++];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else return false;
            code = code * 16U + digit;
        }
        return true;
    }
    bool string(std::string &out) {
        if (!consume('"')) return false;
        out.clear();
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20U) return false;
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= input_.size()) return false;
            switch (input_[pos_++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned code = 0;
                if (!hex4(code)) return false;
                if (code >= 0xd800U && code <= 0xdbffU) {
                    if (input_.substr(pos_, 2) != "\\u") return false;
                    pos_ += 2;
                    unsigned low = 0;
                    if (!hex4(low) || low < 0xdc00U || low > 0xdfffU) return false;
                    code = 0x10000U + ((code - 0xd800U) << 10U) + (low - 0xdc00U);
                } else if (code >= 0xdc00U && code <= 0xdfffU) return false;
                utf8(out, code); break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool nullable_string(std::string &out) {
        whitespace();
        if (pos_ < input_.size() && input_[pos_] == 'n') { out.clear(); return literal("null"); }
        return string(out);
    }
    bool number(std::string_view &out) {
        whitespace(); const std::size_t begin = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '0') ++pos_;
        else {
            if (input_[pos_] < '1' || input_[pos_] > '9') return false;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (pos_ < input_.size() && input_[pos_] == '.') {
            const std::size_t digits = ++pos_;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
            if (digits == pos_) return false;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            const std::size_t digits = pos_;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
            if (digits == pos_) return false;
        }
        out = input_.substr(begin, pos_ - begin); return true;
    }
    bool integer(int &out) {
        whitespace();
        if (pos_ < input_.size() && input_[pos_] == 'n') { out = 0; return literal("null"); }
        std::string_view value;
        if (!number(value)) return false;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) out = 0;
        return true;
    }
    bool skip_value(unsigned depth) {
        if (depth > 32) return false;
        whitespace(); if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '"') { std::string unused; return string(unused); }
        if (input_[pos_] == '{') {
            ++pos_; if (consume('}')) return true;
            do { std::string key; if (!string(key) || !consume(':') || !skip_value(depth + 1)) return false; } while (consume(','));
            return consume('}');
        }
        if (input_[pos_] == '[') {
            ++pos_; if (consume(']')) return true;
            do { if (!skip_value(depth + 1)) return false; } while (consume(','));
            return consume(']');
        }
        if (input_[pos_] == 'n') return literal("null");
        if (input_[pos_] == 't') return literal("true");
        if (input_[pos_] == 'f') return literal("false");
        std::string_view unused; return number(unused);
    }
    std::string_view input_;
    std::size_t pos_ = 0;
};
}  // namespace

RadioBrowserClient::~RadioBrowserClient()
{
    cancel();
    // SIGKILL has already been sent; reap our children during final teardown.
    for (const pid_t pid : cancelled_pids_) while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
}

bool RadioBrowserClient::search(const RadioBrowserQuery &query)
{
    cancel();
    reap_cancelled();
    result_ready_ = false;
    pending_result_ = {};
    payload_.clear();
    if (query.name.size() > 1024) { fail("Search text is too long"); return false; }
    limit_ = std::clamp<std::size_t>(query.limit, 1, 100);
    query_parameters_ = "?limit=" + std::to_string(limit_ + 1) +
                      "&offset=" + std::to_string(query.offset) + "&hidebroken=true&order=clickcount&reverse=true";
    if (!query.name.empty()) query_parameters_ += "&name=" + encode_query(query.name);
    server_index_ = 0;
    return start_request();
}

bool RadioBrowserClient::start_request()
{
    payload_.clear();
    const std::string curl = find_executable("curl");
    if (curl.empty()) { fail("curl not found. Install curl to load stations."); return false; }
    const std::string url = std::string(kEndpoints[server_index_]) + query_parameters_;

    int descriptors[2]{};
    if (pipe(descriptors) != 0) { fail("Unable to create catalog request pipe"); return false; }
    if (fcntl(descriptors[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0) {
        close(descriptors[0]); close(descriptors[1]);
        fail("Unable to configure catalog request pipe"); return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]); close(descriptors[1]);
        fail("Unable to start catalog request"); return false;
    }
    if (child == 0) {
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(126);
        close(descriptors[0]); close(descriptors[1]);
        const int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) { (void)dup2(null_fd, STDIN_FILENO); (void)dup2(null_fd, STDERR_FILENO); close(null_fd); }
        execl(curl.c_str(), curl.c_str(), "--disable", "--location", "--silent", "--show-error", "--fail",
              "--proto", "=https", "--proto-redir", "=https", "--max-time", "12", "--connect-timeout", "5",
              "--max-filesize", "2097152", "--user-agent", "lilygo-ui-radio/1.0",
              url.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    close(descriptors[1]);
    child_pid_ = child;
    child_exited_ = false;
    child_status_ = 0;
    output_fd_ = descriptors[0];
    loading_ = true;
    return true;
}

void RadioBrowserClient::cancel() noexcept
{
    if (output_fd_ >= 0) { close(output_fd_); output_fd_ = -1; }
    if (child_pid_ > 0) {
        (void)kill(child_pid_, SIGKILL);
        const pid_t reaped = waitpid(child_pid_, nullptr, WNOHANG);
        if (reaped == 0 || (reaped < 0 && errno != ECHILD)) cancelled_pids_.push_back(child_pid_);
        child_pid_ = -1;
    }
    loading_ = false;
    result_ready_ = false;
}

void RadioBrowserClient::reap_cancelled() noexcept
{
    cancelled_pids_.erase(std::remove_if(cancelled_pids_.begin(), cancelled_pids_.end(), [](pid_t pid) {
        const pid_t result = waitpid(pid, nullptr, WNOHANG);
        return result == pid || (result < 0 && errno == ECHILD);
    }), cancelled_pids_.end());
}

void RadioBrowserClient::fail(std::string error)
{
    cancel();
    pending_result_ = {};
    pending_result_.error = std::move(error);
    result_ready_ = true;
}

void RadioBrowserClient::retry_or_fail(std::string error)
{
    cancel();
    if (++server_index_ < kEndpoints.size()) (void)start_request();
    else fail(std::move(error));
}

bool RadioBrowserClient::poll(RadioBrowserResult &result)
{
    reap_cancelled();
    if (loading_) {
        char buffer[8192];
        std::size_t received = 0;
        while (output_fd_ >= 0 && received < kReadBudget) {
            const ssize_t count = read(output_fd_, buffer, sizeof(buffer));
            if (count > 0) {
                received += static_cast<std::size_t>(count);
                if (payload_.size() + static_cast<std::size_t>(count) > kMaximumPayload) { retry_or_fail("Radio Browser returned too much data"); break; }
                payload_.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
            if (count == 0) { close(output_fd_); output_fd_ = -1; break; }
            retry_or_fail("Unable to read Radio Browser response"); break;
        }
        if (loading_ && !child_exited_) {
            const pid_t child = waitpid(child_pid_, &child_status_, WNOHANG);
            if (child == child_pid_) { child_pid_ = -1; child_exited_ = true; }
            else if (child < 0 && errno != EINTR) fail("Unable to finish Radio Browser request");
        }
        if (loading_ && child_exited_ && output_fd_ < 0) {
            loading_ = false;
            if (!WIFEXITED(child_status_) || WEXITSTATUS(child_status_) != 0) {
                retry_or_fail("Unable to reach Radio Browser. Check your network and try again.");
            } else {
                RadioBrowserResult parsed;
                if (!StationJsonReader(payload_).read(parsed.stations, limit_, parsed.has_more)) retry_or_fail("Radio Browser returned an invalid response");
                else {
                    pending_result_ = std::move(parsed);
                    result_ready_ = true;
                }
            }
            payload_.clear();
        }
    }
    if (!result_ready_) return false;
    result = std::move(pending_result_);
    pending_result_ = {};
    result_ready_ = false;
    return true;
}

std::string RadioBrowserClient::find_executable(std::string_view name)
{
    const char *path = std::getenv("PATH");
    if (!path || path[0] == '\0') path = "/usr/local/bin:/usr/bin:/bin";
    std::string_view paths(path);
    while (true) {
        const std::size_t separator = paths.find(':');
        std::string directory(paths.substr(0, separator));
        if (directory.empty()) directory = ".";
        const std::string candidate = directory + "/" + std::string(name);
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
        if (separator == std::string_view::npos) break;
        paths.remove_prefix(separator + 1U);
    }
    return {};
}

std::string RadioBrowserClient::encode_query(std::string_view query)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(query.size() * 3U);
    for (const unsigned char c : query) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%'); encoded.push_back(hex[c >> 4U]); encoded.push_back(hex[c & 0x0fU]);
        }
    }
    return encoded;
}
