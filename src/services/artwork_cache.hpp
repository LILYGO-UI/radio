#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Artwork {
    unsigned width = 0;
    unsigned height = 0;
    // Straight-alpha BGRA, matching LVGL ARGB8888 on little-endian targets.
    std::vector<std::uint8_t> pixels;
};

class ArtworkCache {
public:
    ArtworkCache();
    // Disk entries preserve the original encoded image bytes and format.
    // An empty directory disables persistent caching.
    explicit ArtworkCache(std::string cache_directory);
    ~ArtworkCache();
    ArtworkCache(const ArtworkCache &) = delete;
    ArtworkCache &operator=(const ArtworkCache &) = delete;

    // Call these methods from the UI thread. Images are 256x256 with transparent
    // padding. A returned image stays valid until release(url), clear(), or
    // destruction; published pixels are never implicitly evicted.
    const Artwork *request(const std::string &url, bool priority = false);
    bool poll();
    bool loading() const;
    // Unbind all image descriptors using this URL before releasing its pixels.
    // Pending entries are retained and return false; absent entries return true.
    bool release(const std::string &url);
    // Cancels pending work without waiting; preserves the persistent cache.
    void clear();

    static std::string default_cache_directory();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
