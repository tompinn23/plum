#include "journal/sql.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include <sqlite3.h>

namespace journal::sql {
    namespace {
        [[noreturn]] void fail(sqlite3 *db, std::string_view what) {
            throw error(std::format("{}: {}", what, db ? sqlite3_errmsg(db) : "out of memory"));
        }
    } // namespace

    // ── statement ───────────────────────────────────────────────────────────────

    statement::statement(sqlite3 *db, std::string_view sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr) != SQLITE_OK)
            fail(db, std::format("cannot prepare `{}`", sql));
        bound_.assign(static_cast<std::size_t>(sqlite3_bind_parameter_count(stmt_)), false);
    }

    statement::~statement() {
        sqlite3_finalize(stmt_);
    }

    statement::statement(statement &&other) noexcept
        : db_(other.db_), stmt_(std::exchange(other.stmt_, nullptr)), bound_(std::move(other.bound_)),
          started_(other.started_) {
    }

    statement &statement::operator=(statement &&other) noexcept {
        if (this != &other) {
            sqlite3_finalize(stmt_);
            db_ = other.db_;
            stmt_ = std::exchange(other.stmt_, nullptr);
            bound_ = std::move(other.bound_);
            started_ = other.started_;
        }
        return *this;
    }

    int statement::index(std::string_view name) {
        const std::string key = std::format(":{}", name);
        const int i = sqlite3_bind_parameter_index(stmt_, key.c_str());
        if (i == 0) throw error(std::format("no parameter {} in `{}`", key, sqlite3_sql(stmt_)));
        bound_[static_cast<std::size_t>(i - 1)] = true;
        return i;
    }

    statement &statement::bind(std::string_view name, std::nullptr_t) {
        if (sqlite3_bind_null(stmt_, index(name)) != SQLITE_OK) fail(db_, "bind");
        return *this;
    }

    statement &statement::bind(std::string_view name, double value) {
        if (sqlite3_bind_double(stmt_, index(name), value) != SQLITE_OK) fail(db_, "bind");
        return *this;
    }

    statement &statement::bind(std::string_view name, std::string_view value) {
        if (sqlite3_bind_text64(stmt_, index(name), value.data(), value.size(), SQLITE_TRANSIENT,
                                SQLITE_UTF8) != SQLITE_OK)
            fail(db_, "bind");
        return *this;
    }

    statement &statement::bind_integer(std::string_view name, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_, index(name), value) != SQLITE_OK) fail(db_, "bind");
        return *this;
    }

    void statement::check_bound() {
        for (std::size_t i = 0; i < bound_.size(); ++i) {
            if (!bound_[i]) {
                const char *name = sqlite3_bind_parameter_name(stmt_, static_cast<int>(i + 1));
                throw error(std::format("parameter {} is not bound in `{}`", name ? name : "?",
                                        sqlite3_sql(stmt_)));
            }
        }
    }

    bool statement::step() {
        if (!started_) {
            check_bound();
            started_ = true;
        }
        switch (sqlite3_step(stmt_)) {
            case SQLITE_ROW: return true;
            case SQLITE_DONE: return false;
            default: fail(db_, std::format("cannot run `{}`", sqlite3_sql(stmt_)));
        }
    }

    void statement::run() {
        while (step()) {
        }
    }

    void statement::reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
        std::fill(bound_.begin(), bound_.end(), false);
        started_ = false;
    }

    bool statement::null(int column) const {
        return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
    }

    std::int64_t statement::integer(int column) const {
        return sqlite3_column_int64(stmt_, column);
    }

    double statement::real(int column) const {
        return sqlite3_column_double(stmt_, column);
    }

    std::string statement::text(int column) const {
        const auto *p = reinterpret_cast<const char *>(sqlite3_column_text(stmt_, column));
        return p ? std::string(p, static_cast<std::size_t>(sqlite3_column_bytes(stmt_, column))) : std::string();
    }

    std::optional<std::int64_t> statement::opt_integer(int column) const {
        if (null(column)) return std::nullopt;
        return integer(column);
    }

    std::optional<double> statement::opt_real(int column) const {
        if (null(column)) return std::nullopt;
        return real(column);
    }

    std::optional<std::string> statement::opt_text(int column) const {
        if (null(column)) return std::nullopt;
        return text(column);
    }

    std::optional<timestamp> statement::opt_time(int column) const {
        if (null(column)) return std::nullopt;
        return parse_timestamp(text(column));
    }

    // ── database ────────────────────────────────────────────────────────────────

    database::database(const std::filesystem::path &file) {
        const auto utf8 = file.u8string();
        const std::string name(utf8.begin(), utf8.end());
        if (sqlite3_open_v2(name.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            std::string message = std::format("cannot open {}: {}", name, db_ ? sqlite3_errmsg(db_) : "out of memory");
            sqlite3_close(db_);
            db_ = nullptr;
            throw error(message);
        }
        sqlite3_extended_result_codes(db_, 1);
        sqlite3_busy_timeout(db_, 5000);
    }

    database::~database() {
        sqlite3_close_v2(db_);
    }

    database::database(database &&other) noexcept : db_(std::exchange(other.db_, nullptr)) {
    }

    database &database::operator=(database &&other) noexcept {
        if (this != &other) {
            sqlite3_close_v2(db_);
            db_ = std::exchange(other.db_, nullptr);
        }
        return *this;
    }

    void database::exec(std::string_view sql) {
        const std::string text(sql);
        char *message = nullptr;
        if (sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
            std::string reason = message ? message : sqlite3_errmsg(db_);
            sqlite3_free(message);
            throw error(std::format("cannot run `{}`: {}", sql, reason));
        }
    }

    statement database::prepare(std::string_view sql) {
        return statement(db_, sql);
    }

    std::int64_t database::last_insert_rowid() const {
        return sqlite3_last_insert_rowid(db_);
    }

    bool database::in_transaction() const {
        return db_ && sqlite3_get_autocommit(db_) == 0;
    }
} // namespace journal::sql
