#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "journal/game_event.hpp"
#include "journal/game_state.hpp"
#include "journal/journal_service.hpp"

namespace journal {
    // "$Hydrogen_Fuel_Name;" -> "hydrogen_fuel"; anything else lowercased.
    std::string canonicalise(std::string_view raw);

    // "$MICRORESOURCE_CATEGORY_Data;" -> "Data"; anything else unchanged.
    std::string category(std::string_view raw);

    // Canonical ship symbol -> display name, e.g. "python_nx" -> "Python Mk II".
    std::optional<std::string> ship_display_name(std::string_view canonical);

    // Folds journal events into a game_state, one at a time.
    class journal_parser {
    public:
        // For an event whose real content lives in a sibling file (Market.json, Cargo.json, ...).
        using delegate = std::function<game_event(const game_event &)>;

        [[nodiscard]] const game_state &state() const { return state_; }

        // Only called in phase::live: during replay the sibling file describes now, not then.
        void handler(std::string event_name, delegate fn);

        // Folds one event in. Returns the event, or an enriched copy where the parser could add
        // something the raw line lacked (carrier events gain a Callsign).
        game_event parse(const game_event &event, phase p);

    private:
        game_event delegate_to(const game_event &event);

        void clear_location();

        void consume_material(const std::string &name, int count);

        std::map<std::string, int> *material_map(std::string_view category);

        void set_star_pos(const game_event &event);

        void set_version_info(const game_event &event);

        void apply_backpack_change(const json &row, int sign);

        void start_session();

        game_state state_;
        std::optional<game_event> header_; // this file's Fileheader, applied when a session starts
        bool session_started_ = false;
        std::unordered_map<std::int64_t, std::string> carrier_callsigns_;
        std::unordered_map<std::string, delegate> handlers_;
    };
} // namespace journal
