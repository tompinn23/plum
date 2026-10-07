#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace journal {
    using json = nlohmann::json;

    // Journal timestamps are whole seconds, UTC.
    using timestamp = std::chrono::sys_seconds;

    // "2024-01-02T03:04:05Z" -> timestamp; nullopt if the text is not in that shape.
    std::optional<timestamp> parse_timestamp(std::string_view text);

    // timestamp -> "2024-01-02T03:04:05Z". Fixed width, so stored values sort as text.
    std::string format_timestamp(timestamp t);

    // Readers over a JSON object: a whole event payload, or a nested row such as a Modules entry.
    // Journals distinguish "field omitted" from "field is zero", so the opt_ readers return nullopt
    // where the plain ones return the fallback. A field of the wrong type counts as omitted.
    namespace field {
        std::string str(const json &row, std::string_view key, std::string_view fallback = {});

        std::int64_t num(const json &row, std::string_view key, std::int64_t fallback = 0);

        double dec(const json &row, std::string_view key, double fallback = 0.0);

        bool flag(const json &row, std::string_view key, bool fallback = false);

        std::optional<std::string> opt_str(const json &row, std::string_view key);

        std::optional<std::int64_t> opt_long(const json &row, std::string_view key);

        std::optional<double> opt_double(const json &row, std::string_view key);

        std::optional<bool> opt_bool(const json &row, std::string_view key);

        bool has(const json &row, std::string_view key);

        // The array or object at key, or a shared empty one, so callers can iterate unconditionally.
        const json &rows(const json &row, std::string_view key);

        const json &object(const json &row, std::string_view key);

        std::vector<std::string> strings(const json &row, std::string_view key);

        std::vector<double> doubles(const json &row, std::string_view key);
    } // namespace field

    // One journal line.
    class game_event {
    public:
        game_event() = default;

        explicit game_event(json payload);

        // nullopt for a line that is not a JSON object, which journals do occasionally contain
        // where the game was killed mid-write.
        static std::optional<game_event> parse(std::string_view line);

        [[nodiscard]] const std::string &name() const { return name_; }
        [[nodiscard]] std::optional<timestamp> time() const { return time_; }
        [[nodiscard]] const json &payload() const { return payload_; }

        // This event with one payload key added or replaced; the original is untouched.
        [[nodiscard]] game_event with(std::string_view key, json value) const;

        [[nodiscard]] std::string str(std::string_view key, std::string_view fallback = {}) const {
            return field::str(payload_, key, fallback);
        }

        [[nodiscard]] std::int64_t num(std::string_view key, std::int64_t fallback = 0) const {
            return field::num(payload_, key, fallback);
        }

        [[nodiscard]] double dec(std::string_view key, double fallback = 0.0) const {
            return field::dec(payload_, key, fallback);
        }

        [[nodiscard]] bool flag(std::string_view key, bool fallback = false) const {
            return field::flag(payload_, key, fallback);
        }

        [[nodiscard]] std::optional<std::string> opt_str(std::string_view key) const {
            return field::opt_str(payload_, key);
        }

        [[nodiscard]] std::optional<std::int64_t> opt_long(std::string_view key) const {
            return field::opt_long(payload_, key);
        }

        [[nodiscard]] std::optional<double> opt_double(std::string_view key) const {
            return field::opt_double(payload_, key);
        }

        [[nodiscard]] std::optional<bool> opt_bool(std::string_view key) const {
            return field::opt_bool(payload_, key);
        }

        [[nodiscard]] bool has(std::string_view key) const { return field::has(payload_, key); }
        [[nodiscard]] const json &rows(std::string_view key) const { return field::rows(payload_, key); }
        [[nodiscard]] const json &object(std::string_view key) const { return field::object(payload_, key); }

        [[nodiscard]] std::vector<std::string> strings(std::string_view key) const {
            return field::strings(payload_, key);
        }

        [[nodiscard]] std::vector<double> doubles(std::string_view key) const {
            return field::doubles(payload_, key);
        }

    private:
        json payload_ = json::object();
        std::string name_;
        std::optional<timestamp> time_;
    };
} // namespace journal
