#pragma once

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "journal/game_event.hpp"

struct sqlite3;
struct sqlite3_stmt;

// A thin RAII layer over the SQLite C API. Parameters are SQLite's own `:name` placeholders;
// binding a name the statement does not have, or stepping with one left unbound, throws rather
// than silently writing NULL.
namespace journal::sql {

class error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class statement {
public:
    statement(sqlite3* db, std::string_view sql);
    ~statement();
    statement(statement&& other) noexcept;
    statement& operator=(statement&& other) noexcept;
    statement(const statement&) = delete;
    statement& operator=(const statement&) = delete;

    // `name` without the leading colon.
    statement& bind(std::string_view name, std::nullptr_t);
    statement& bind(std::string_view name, double value);
    statement& bind(std::string_view name, std::string_view value);
    statement& bind(std::string_view name, const char* value) { return bind(name, std::string_view(value)); }
    statement& bind(std::string_view name, const std::string& value) { return bind(name, std::string_view(value)); }
    statement& bind(std::string_view name, bool value) { return bind_integer(name, value ? 1 : 0); }
    statement& bind(std::string_view name, timestamp value) { return bind(name, format_timestamp(value)); }

    template <std::integral T>
        requires(!std::same_as<T, bool>)
    statement& bind(std::string_view name, T value) {
        return bind_integer(name, static_cast<std::int64_t>(value));
    }

    template <class T>
    statement& bind(std::string_view name, const std::optional<T>& value) {
        return value ? bind(name, *value) : bind(name, nullptr);
    }

    // Advances to the next row: true while one is available, false once done.
    bool step();

    // Steps to completion, for statements that return nothing worth reading.
    void run();

    // Rewinds for re-use and forgets every binding.
    void reset();

    [[nodiscard]] bool null(int column) const;
    [[nodiscard]] std::int64_t integer(int column) const;
    [[nodiscard]] double real(int column) const;
    [[nodiscard]] std::string text(int column) const;
    [[nodiscard]] std::optional<std::int64_t> opt_integer(int column) const;
    [[nodiscard]] std::optional<double> opt_real(int column) const;
    [[nodiscard]] std::optional<std::string> opt_text(int column) const;
    [[nodiscard]] std::optional<timestamp> opt_time(int column) const;

private:
    statement& bind_integer(std::string_view name, std::int64_t value);
    int index(std::string_view name);
    void check_bound();

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    std::vector<bool> bound_;
    bool started_ = false;
};

class database {
public:
    // Opens read-write, creating the file if needed.
    explicit database(const std::filesystem::path& file);
    ~database();
    database(database&& other) noexcept;
    database& operator=(database&& other) noexcept;
    database(const database&) = delete;
    database& operator=(const database&) = delete;

    // Runs one or more statements that take no parameters.
    void exec(std::string_view sql);

    [[nodiscard]] statement prepare(std::string_view sql);

    [[nodiscard]] std::int64_t last_insert_rowid() const;
    [[nodiscard]] bool in_transaction() const;
    [[nodiscard]] sqlite3* handle() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

}  // namespace journal::sql
