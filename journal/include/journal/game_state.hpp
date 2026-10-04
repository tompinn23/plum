#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace journal {

struct rank_level {
    int rank = 0;
    int progress = 0;  // percent toward the next rank
};

// Either unlocked (rank set) or not yet ("Invited", "Known", ...).
struct engineer_progress {
    std::string state;
    std::optional<int> rank;
    std::optional<int> rank_progress;
};

struct module_modifier {
    std::string label;
    std::optional<std::int64_t> less_is_good;
    std::optional<double> original_value;
    std::optional<double> value;
    std::optional<std::string> value_str;
    std::optional<std::string> value_str_localised;
};

struct module_engineering {
    std::int64_t blueprint_id = 0;
    std::string blueprint_name;
    std::optional<std::string> engineer;
    std::int64_t engineer_id = 0;
    std::optional<std::string> experimental_effect;
    std::optional<std::string> experimental_effect_localised;
    std::int64_t level = 0;
    double quality = 0.0;
    std::vector<module_modifier> modifiers;
};

struct ship_module {
    std::string slot;
    std::string item;  // canonical symbol
    bool on = true;
    std::int64_t priority = 0;
    double health = 1.0;
    std::optional<std::int64_t> value;
    std::optional<std::int64_t> ammo_in_clip;
    std::optional<std::int64_t> ammo_in_hopper;
    std::optional<module_engineering> engineering;
};

struct backpack_contents {
    std::map<std::string, int> component;
    std::map<std::string, int> consumable;
    std::map<std::string, int> item;
    std::map<std::string, int> data;

    void clear() {
        component.clear();
        consumable.clear();
        item.clear();
        data.clear();
    }
};

struct powerplay_info {
    std::optional<std::string> power;
    std::optional<int> rank;
    std::optional<std::int64_t> merits;
    std::optional<std::int64_t> votes;
    std::optional<std::int64_t> time_pledged;
};

struct carrier_info {
    std::string name;
    std::string callsign;
    std::string system_name;
};

struct fuel_tanks {
    double main = 0.0;
    double reserve = 0.0;
};

// Everything the parser knows about the commander right now. A plain value: copying it is a
// deep copy, which is how snapshots are published across threads.
struct game_state {
    // File header
    std::optional<std::string> game_language;
    std::optional<std::string> game_version;
    std::optional<std::string> game_build;

    // Profile
    std::optional<std::string> name;
    std::optional<std::string> fid;
    std::optional<std::string> group;
    std::string mode;
    std::int64_t credits = 0;
    std::optional<bool> horizons;
    bool odyssey = false;
    std::optional<std::int64_t> loan;

    // Inventory, keyed by canonical symbol
    std::map<std::string, int> cargo;
    std::map<std::string, int> raw;
    std::map<std::string, int> manufactured;
    std::map<std::string, int> encoded;
    backpack_contents backpack;

    // Standing
    std::map<std::string, engineer_progress> engineers;
    std::map<std::string, rank_level> rank;          // "Combat", "Trade", "Explore", ...
    std::map<std::string, double> reputation;        // "Federation", "Empire", ...
    powerplay_info powerplay;

    // Crew
    std::optional<std::string> captain;
    std::optional<std::string> role;
    std::set<std::string> friends;

    // Ship
    std::optional<std::int64_t> ship_id;
    std::optional<std::string> ship_ident;
    std::optional<std::string> ship_name;
    std::optional<std::string> ship_type;       // canonical symbol, e.g. "python_nx"
    std::optional<std::string> ship_localised;  // display name, e.g. "Python Mk II"
    std::optional<std::int64_t> hull_value;
    std::optional<std::int64_t> modules_value;
    std::optional<std::int64_t> rebuy;
    std::optional<std::int64_t> cargo_capacity;
    std::optional<double> unladen_mass;
    std::optional<double> max_jump_range;
    std::optional<fuel_tanks> fuel_capacity;
    std::map<std::string, ship_module> modules;  // keyed by slot

    // Flags
    bool is_docked = false;
    bool on_foot = false;
    bool taxi = false;
    bool dropship = false;

    // Location
    std::optional<std::array<double, 3>> star_pos;
    std::optional<std::int64_t> system_address;
    std::optional<std::string> system_name;
    std::optional<std::int64_t> system_population;
    std::optional<std::string> body;
    std::optional<std::int64_t> body_id;
    std::optional<std::string> body_type;
    std::optional<std::string> station_name;
    std::optional<std::string> station_type;
    std::optional<std::int64_t> market_id;
    std::vector<std::string> station_services;

    std::optional<carrier_info> carrier;

    carrier_info& carrier_or_create() {
        if (!carrier) carrier.emplace();
        return *carrier;
    }
};

}  // namespace journal
