#pragma once

#include "product/media_acquire/source.h"

#include <cstddef>
#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ninfer::product::media_acquire {

enum class ErrorKind {
    BudgetExceeded,
    RemoteUnavailable,
    RemoteTimeout,
    DeadlineExceeded,
    Cancelled,
};

class Error final : public std::runtime_error {
public:
    Error(ErrorKind kind, std::string message)
        : std::runtime_error(std::move(message)), kind_(kind) {}

    [[nodiscard]] ErrorKind kind() const noexcept { return kind_; }

private:
    ErrorKind kind_;
};

struct Policy {
    std::size_t max_bytes      = 256ULL << 20;
    int connect_timeout_ms     = 5'000;
    int timeout_ms             = 60'000;
    int max_redirects          = 3;
    bool allow_remote          = true;
    bool allow_private_network = false;
    std::filesystem::path media_root;
    std::chrono::steady_clock::time_point deadline;
    std::function<bool()> is_cancelled;
};

std::vector<std::uint8_t> acquire_bytes(const Source& source, const Policy& policy = {});

// Validates a remote media URL (scheme, credentials, remote/private-network policy) and follows
// its redirects with the same check on every hop, fetching at most one byte of each; returns the
// final URL, for readers that stream it themselves with seeks (HTTP range requests) and must not
// follow further redirects.
std::string resolve_remote_url(std::string url, const Policy& policy);

// Canonical path of a Path source after the media-root check, without reading the file.
std::filesystem::path resolve_media_path(const Source& source, const Policy& policy);

} // namespace ninfer::product::media_acquire
