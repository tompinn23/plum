#include "journal/game_event.hpp"

#include <format>

namespace journal {
    namespace {
        bool digits(std::string_view s, std::size_t pos, std::size_t count, int &out) {
            out = 0;
            for (std::size_t i = pos; i < pos + count; ++i) {
                const char c = s[i];
                if (c < '0' || c > '9') return false;
                out = out * 10 + (c - '0');
            }
            return true;
        }

        // The value at key, or null for an absent key, an explicit JSON null, or a non-object row.
        const json *lookup(const json &row, std::string_view key) {
            if (!row.is_object()) return nullptr;
            const auto it = row.find(key);
            if (it == row.end() || it->is_null()) return nullptr;
            return &*it;
        }

        const json &empty_array() {
            static const json value = json::array();
            return value;
        }

        const json &empty_object() {
            static const json value = json::object();
            return value;
        }
    } // namespace

    std::optional<timestamp> parse_timestamp(std::string_view s) {
        using namespace std::chrono;
        // YYYY-MM-DDTHH:MM:SS, then a 'Z' or fractional seconds we ignore.
        if (s.size() < 19) return std::nullopt;
        int y, mo, d, h, mi, se;
        if (!digits(s, 0, 4, y) || s[4] != '-' || !digits(s, 5, 2, mo) || s[7] != '-' ||
            !digits(s, 8, 2, d) || (s[10] != 'T' && s[10] != ' ') || !digits(s, 11, 2, h) ||
            s[13] != ':' || !digits(s, 14, 2, mi) || s[16] != ':' || !digits(s, 17, 2, se)) {
            return std::nullopt;
        }
        const year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)}, day{static_cast<unsigned>(d)}};
        if (!ymd.ok() || h > 23 || mi > 59 || se > 60) return std::nullopt;
        return sys_days{ymd} + hours{h} + minutes{mi} + seconds{se};
    }

    std::string format_timestamp(timestamp t) {
        return std::format("{:%FT%TZ}", t);
    }

    namespace field {
        std::string str(const json &row, std::string_view key, std::string_view fallback) {
            const json *v = lookup(row, key);
            return v && v->is_string() ? v->get<std::string>() : std::string(fallback);
        }

        std::int64_t num(const json &row, std::string_view key, std::int64_t fallback) {
            return opt_long(row, key).value_or(fallback);
        }

        double dec(const json &row, std::string_view key, double fallback) {
            return opt_double(row, key).value_or(fallback);
        }

        bool flag(const json &row, std::string_view key, bool fallback) {
            return opt_bool(row, key).value_or(fallback);
        }

        std::optional<std::string> opt_str(const json &row, std::string_view key) {
            const json *v = lookup(row, key);
            if (v && v->is_string()) return v->get<std::string>();
            return std::nullopt;
        }

        std::optional<std::int64_t> opt_long(const json &row, std::string_view key) {
            const json *v = lookup(row, key);
            if (!v) return std::nullopt;
            if (v->is_number_unsigned()) return static_cast<std::int64_t>(v->get<std::uint64_t>());
            if (v->is_number_integer()) return v->get<std::int64_t>();
            if (v->is_number_float()) return static_cast<std::int64_t>(v->get<double>());
            return std::nullopt;
        }

        std::optional<double> opt_double(const json &row, std::string_view key) {
            const json *v = lookup(row, key);
            if (v && v->is_number()) return v->get<double>();
            return std::nullopt;
        }

        std::optional<bool> opt_bool(const json &row, std::string_view key) {
            const json *v = lookup(row, key);
            if (v && v->is_boolean()) return v->get<bool>();
            return std::nullopt;
        }

        bool has(const json &row, std::string_view key) {
            return lookup(row, key) != nullptr;
        }

        const json &rows(const json &row, std::string_view key) {
            const json *v = lookup(row, key);
            return v && v->is_array() ? *v : empty_array();
        }

        const json &object(const json &row, std::string_view key) {
            const json *v = lookup(row, key);
            return v && v->is_object() ? *v : empty_object();
        }

        std::vector<std::string> strings(const json &row, std::string_view key) {
            std::vector<std::string> out;
            for (const auto &v: rows(row, key))
                if (v.is_string()) out.push_back(v.get<std::string>());
            return out;
        }

        std::vector<double> doubles(const json &row, std::string_view key) {
            std::vector<double> out;
            for (const auto &v: rows(row, key))
                if (v.is_number()) out.push_back(v.get<double>());
            return out;
        }
    } // namespace field

    game_event::game_event(json payload) : payload_(std::move(payload)) {
        if (!payload_.is_object()) payload_ = json::object();
        name_ = field::str(payload_, "event");
        if (const auto ts = field::opt_str(payload_, "timestamp")) time_ = parse_timestamp(*ts);
    }

    std::optional<game_event> game_event::parse(std::string_view line) {
        json parsed = json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_object()) return std::nullopt; // also catches the discarded (invalid) value
        return game_event(std::move(parsed));
    }

    game_event game_event::with(std::string_view key, json value) const {
        json copy = payload_;
        copy[std::string(key)] = std::move(value);
        return game_event(std::move(copy));
    }
} // namespace journal
