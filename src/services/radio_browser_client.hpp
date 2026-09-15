#ifndef LILYGO_UI_RADIO_SERVICES_RADIO_BROWSER_CLIENT_HPP
#define LILYGO_UI_RADIO_SERVICES_RADIO_BROWSER_CLIENT_HPP

#include "pages/radio/radio_model.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

struct RadioBrowserQuery {
    std::string name;
    std::size_t offset = 0;
    std::size_t limit = 50;
};

struct RadioBrowserResult {
    std::vector<RadioStation> stations;
    std::string error;
    bool has_more = false;
};

class RadioBrowserClientInterface {
public:
    virtual ~RadioBrowserClientInterface() = default;
    virtual bool search(const RadioBrowserQuery &query) = 0;
    /** Return a completed result once; never wait for network or process exit. */
    virtual bool poll(RadioBrowserResult &result) = 0;
    [[nodiscard]] virtual bool loading() const noexcept = 0;
};

/** Queries Radio Browser through a child curl process and a nonblocking pipe. */
class RadioBrowserClient final : public RadioBrowserClientInterface {
public:
    RadioBrowserClient() = default;
    ~RadioBrowserClient() override;

    RadioBrowserClient(const RadioBrowserClient &)            = delete;
    RadioBrowserClient &operator=(const RadioBrowserClient &) = delete;

    bool search(const RadioBrowserQuery &query) override;
    bool poll(RadioBrowserResult &result) override;
    [[nodiscard]] bool loading() const noexcept override { return loading_; }
    void cancel() noexcept;

private:
    static std::string find_executable(std::string_view name);
    static std::string encode_query(std::string_view query);
    bool start_request();
    void retry_or_fail(std::string error);
    void fail(std::string error);
    void reap_cancelled() noexcept;

    pid_t child_pid_ = -1;
    int output_fd_ = -1;
    int child_status_ = 0;
    bool child_exited_ = false;
    bool loading_ = false;
    bool result_ready_ = false;
    std::size_t limit_ = 50;
    std::size_t server_index_ = 0;
    std::string query_parameters_;
    std::string payload_;
    RadioBrowserResult pending_result_;
    std::vector<pid_t> cancelled_pids_;
};

#endif
