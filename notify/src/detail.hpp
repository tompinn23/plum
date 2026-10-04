// Internal helpers shared by the backends.
#pragma once

#include "notify/notify.hpp"

#include <filesystem>
#include <system_error>
#include <vector>


namespace notify::detail {

inline event make_event(const event_kind kind, std::vector<std::filesystem::path> paths,
                        const modify_kind modify = modify_kind::Any, const bool need_rescan = false) {
    event e;
    e.kind = kind;
    e.paths = std::move(paths);
    e.modify = modify;
    e.need_rescan = need_rescan;
    return e;
}

// Absolute, normalised, no trailing separator, so watch("a/") and unwatch("./a")
// refer to the same entry.
inline std::filesystem::path normalize(const std::filesystem::path& p, std::error_code& ec) {
    auto abs = std::filesystem::absolute(p, ec);
    if (ec) return {};
    abs = abs.lexically_normal();
    if (!abs.has_filename() && abs != abs.root_path()) abs = abs.parent_path();
    return abs;
}

} // namespace notify::detail


