#include "journal_parser.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace journal {
    namespace {
        std::string lower(std::string_view s) {
            std::string out(s);
            std::ranges::transform(out, out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        bool iequals_prefix(std::string_view s, std::string_view prefix) {
            return s.size() >= prefix.size() && lower(s.substr(0, prefix.size())) == lower(prefix);
        }

        bool iequals_suffix(std::string_view s, std::string_view suffix) {
            return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == lower(suffix);
        }

        // Applies a signed delta, dropping the key once it reaches zero.
        void adjust(std::map<std::string, int> &map, const std::string &key, int delta) {
            if (key.empty() || delta == 0) return;
            if ((map[key] += delta) <= 0) map.erase(key);
        }

        // As adjust, but never creates a key that was not already there.
        void adjust_existing(std::map<std::string, int> &map, const std::string &key, int delta) {
            const auto it = map.find(key);
            if (it == map.end()) return;
            if ((it->second += delta) <= 0) map.erase(it);
        }

        int count(const json &row, std::string_view key) {
            return static_cast<int>(field::num(row, key, 0));
        }

        module_modifier modifier_from(const json &row) {
            return {
                .label = field::str(row, "Label"),
                .less_is_good = field::opt_long(row, "LessIsGood"),
                .original_value = field::opt_double(row, "OriginalValue"),
                .value = field::opt_double(row, "Value"),
                .value_str = field::opt_str(row, "ValueStr"),
                .value_str_localised = field::opt_str(row, "ValueStr_Localised"),
            };
        }

        std::optional<module_engineering> engineering_from(const json &row) {
            if (!row.is_object() || row.empty()) return std::nullopt;
            module_engineering e;
            e.blueprint_id = field::num(row, "BlueprintID");
            e.blueprint_name = field::str(row, "BlueprintName");
            e.engineer = field::opt_str(row, "Engineer");
            e.engineer_id = field::num(row, "EngineerID");
            e.experimental_effect = field::opt_str(row, "ExperimentalEffect");
            e.experimental_effect_localised = field::opt_str(row, "ExperimentalEffect_Localised");
            e.level = field::num(row, "Level");
            e.quality = field::dec(row, "Quality");
            for (const auto &mod: field::rows(row, "Modifiers")) e.modifiers.push_back(modifier_from(mod));
            return e;
        }

        ship_module module_from(const json &row) {
            ship_module m;
            m.slot = field::str(row, "Slot");
            m.item = canonicalise(field::str(row, "Item"));
            m.on = field::flag(row, "On", true);
            m.priority = field::num(row, "Priority");
            m.health = field::dec(row, "Health", 1.0);
            m.value = field::opt_long(row, "Value");
            m.ammo_in_clip = field::opt_long(row, "AmmoInClip");
            m.ammo_in_hopper = field::opt_long(row, "AmmoInHopper");
            m.engineering = engineering_from(field::object(row, "Engineering"));
            return m;
        }

        // Folds whichever fields a row carries into an engineer's entry. Rows come in several shapes:
        // the login snapshot lists every engineer with Progress, plus Rank and RankProgress once
        // unlocked; a later update may carry only Progress ("Invited"), or only Rank.
        void merge_engineer(engineer_progress &p, const json &row) {
            if (auto progress = field::opt_str(row, "Progress")) p.state = std::move(*progress);
            if (const auto rank = field::opt_long(row, "Rank")) {
                p.rank = static_cast<int>(*rank);
                if (p.state.empty()) p.state = "Unlocked";
            }
            if (const auto rank_progress = field::opt_long(row, "RankProgress"))
                p.rank_progress = static_cast<int>(*rank_progress);
        }

        std::string system_name(const game_event &event) {
            std::string name = event.str("StarSystem");
            return name == "ProvingGround" ? "CQC" : name;
        }
    } // namespace

    std::string canonicalise(std::string_view raw) {
        if (raw.size() > 7 && raw.front() == '$' && iequals_suffix(raw, "_name;"))
            return lower(raw.substr(1, raw.size() - 7));
        return lower(raw);
    }

    std::string category(std::string_view raw) {
        constexpr std::string_view prefix = "$MICRORESOURCE_CATEGORY_";
        if (raw.size() > prefix.size() + 1 && iequals_prefix(raw, prefix) && raw.back() == ';')
            return std::string(raw.substr(prefix.size(), raw.size() - prefix.size() - 1));
        return std::string(raw);
    }

    std::optional<std::string> ship_display_name(std::string_view canonical) {
        static const std::unordered_map<std::string_view, std::string_view> names = {
            {"adder", "Adder"},
            {"anaconda", "Anaconda"},
            {"asp", "Asp Explorer"},
            {"asp_scout", "Asp Scout"},
            {"belugaliner", "Beluga Liner"},
            {"cobramkiii", "Cobra MkIII"},
            {"cobramkiv", "Cobra MkIV"},
            {"cobramkv", "Cobra Mk V"},
            {"corsair", "Corsair"},
            {"clipper", "Panther Clipper"},
            {"cutter", "Imperial Cutter"},
            {"diamondback", "Diamondback Scout"},
            {"diamondbackxl", "Diamondback Explorer"},
            {"dolphin", "Dolphin"},
            {"eagle", "Eagle"},
            {"explorer_nx", "Caspian Explorer"},
            {"empire_courier", "Imperial Courier"},
            {"empire_eagle", "Imperial Eagle"},
            {"empire_fighter", "Imperial Fighter"},
            {"empire_trader", "Imperial Clipper"},
            {"federation_corvette", "Federal Corvette"},
            {"federation_dropship", "Federal Dropship"},
            {"federation_dropship_mkii", "Federal Assault Ship"},
            {"federation_gunship", "Federal Gunship"},
            {"federation_fighter", "F63 Condor"},
            {"ferdelance", "Fer-de-Lance"},
            {"hauler", "Hauler"},
            {"independant_trader", "Keelback"},
            {"independent_fighter", "Taipan Fighter"},
            {"krait_mkii", "Krait MkII"},
            {"krait_light", "Krait Phantom"},
            {"lakonminer", "Type-11 Prospector"},
            {"mamba", "Mamba"},
            {"mandalay", "Mandalay"},
            {"orca", "Orca"},
            {"panthermkii", "Panther Clipper Mk II"},
            {"python", "Python"},
            {"python_nx", "Python Mk II"},
            {"scout", "Taipan Fighter"},
            {"sidewinder", "Sidewinder"},
            {"testbuggy", "Scarab"},
            {"type6", "Type-6 Transporter"},
            {"type7", "Type-7 Transporter"},
            {"type8", "Type-8 Transporter"},
            {"type9", "Type-9 Heavy"},
            {"type9_military", "Type-10 Defender"},
            {"typex", "Alliance Chieftain"},
            {"typex_2", "Alliance Crusader"},
            {"typex_3", "Alliance Challenger"},
            {"viper", "Viper MkIII"},
            {"viper_mkiv", "Viper MkIV"},
            {"vulture", "Vulture"},
        };
        const auto it = names.find(canonical);
        if (it == names.end()) return std::nullopt;
        return std::string(it->second);
    }

    void journal_parser::handler(std::string event_name, delegate fn) {
        handlers_[std::move(event_name)] = std::move(fn);
    }

    game_event journal_parser::delegate_to(const game_event &event) {
        const auto it = handlers_.find(event.name());
        if (it == handlers_.end()) {
            spdlog::debug("no handler registered for file-backed event {}", event.name());
            return event;
        }
        return it->second(event);
    }

    std::map<std::string, int> *journal_parser::material_map(std::string_view cat) {
        if (cat == "Raw" || cat == "Elements") return &state_.raw;
        if (cat == "Manufactured") return &state_.manufactured;
        if (cat == "Encoded") return &state_.encoded;
        return nullptr;
    }

    // Removes from whichever material map holds the name.
    void journal_parser::consume_material(const std::string &name, int n) {
        adjust_existing(state_.raw, name, -n);
        adjust_existing(state_.manufactured, name, -n);
        adjust_existing(state_.encoded, name, -n);
    }

    void journal_parser::clear_location() {
        state_.star_pos.reset();
        state_.system_name.reset();
        state_.system_address.reset();
        state_.system_population.reset();
        state_.body.reset();
        state_.body_id.reset();
        state_.body_type.reset();
        state_.station_name.reset();
        state_.market_id.reset();
        state_.station_type.reset();
        state_.station_services.clear();
    }

    void journal_parser::set_star_pos(const game_event &event) {
        if (const auto pos = event.doubles("StarPos"); pos.size() == 3)
            state_.star_pos = std::array < double, 3 >
        {
            pos[0], pos[1], pos[2]
        };
    }

    void journal_parser::set_version_info(const game_event &event) {
        // Fileheader and LoadGame spell these the same way; LoadGame only overrides what it carries.
        if (auto v = event.opt_str("language")) state_.game_language = std::move(v);
        if (auto v = event.opt_str("gameversion")) state_.game_version = std::move(v);
        if (auto v = event.opt_str("build")) state_.game_build = std::move(v);
    }

    // A session starts when a commander loads, not at the Fileheader: a game started and quit at the
    // main menu writes a Fileheader and nothing else, and resetting there would blank the state for
    // no new information. Everything after Commander restates the session, so nothing stale survives.
    void journal_parser::start_session() {
        state_ = game_state{};
        if (header_) set_version_info(*header_);
        session_started_ = true;
    }

    void journal_parser::apply_backpack_change(const json &row, int sign) {
        const std::string cat = category(field::str(row, "Type"));
        std::map<std::string, int> *map = cat == "Data"
                                              ? &state_.backpack.data
                                              : cat == "Component"
                                                    ? &state_.backpack.component
                                                    : cat == "Item"
                                                          ? &state_.backpack.item
                                                          : cat == "Consumable"
                                                                ? &state_.backpack.consumable
                                                                : nullptr;
        if (map) adjust(*map, canonicalise(field::str(row, "Name")), sign * count(row, "Count"));
    }

    game_event journal_parser::parse(const game_event &event, phase p) {
        const std::string &name = event.name();
        game_state &s = state_;

        if (name == "Fileheader") {
            header_ = event;
            session_started_ = false;
        } else if (name == "Commander") {
            start_session();
            s.name = event.str("Name");
            s.fid = event.opt_str("FID");
        } else if (name == "LoadGame") {
            if (!session_started_) start_session(); // journals from before the Commander event
            const auto ship = event.opt_str("Ship");
            const auto mode = event.opt_str("GameMode");
            if ((ship && !mode) || (mode && lower(*mode) == "cqc")) {
                s.mode = "CQC";
            } else {
                s.mode = mode.value_or("");
            }
            s.name = event.str("Commander");
            s.group = event.opt_str("Group");
            clear_location();
            s.captain.reset();
            s.credits = event.num("Credits");
            s.fid = event.opt_str("FID");
            s.horizons = event.opt_bool("Horizons");
            s.odyssey = event.flag("Odyssey");
            s.loan = event.opt_long("Loan");
            s.role.reset();
            s.taxi = false;
            s.dropship = false;
            s.on_foot = ship && lower(*ship).find("suit") != std::string::npos;
            set_version_info(event);
        } else if (name == "NewCommander") {
            s.name = event.str("Name");
            s.group.reset();
        } else if (name == "SetUserShipName") {
            s.ship_id = event.opt_long("ShipID");
            s.ship_ident = event.opt_str("UserShipId");
            s.ship_name = event.opt_str("UserShipName");
            const std::string type = canonicalise(event.str("Ship"));
            if (s.ship_type != type) {
                s.ship_localised = ship_display_name(type);
                s.ship_type = type;
            }
        } else if (name == "ShipyardBuy") {
            s.ship_id.reset();
            s.ship_ident.reset();
            s.ship_name.reset();
            s.ship_type = canonicalise(event.str("ShipType"));
            s.ship_localised = event.opt_str("ShipType_Localised");
            s.hull_value.reset();
            s.modules_value.reset();
            s.rebuy.reset();
            s.modules.clear();
            // The old ship can be sold as part of the purchase.
            s.credits += event.num("SellPrice") - event.num("ShipPrice");
        } else if (name == "ShipyardSwap") {
            s.ship_id = event.opt_long("ShipID");
            s.ship_ident.reset();
            s.ship_name.reset();
            s.ship_type = canonicalise(event.str("ShipType"));
            s.ship_localised = event.opt_str("ShipType_Localised");
            s.hull_value.reset();
            s.modules_value.reset();
            s.rebuy.reset();
            s.modules.clear();
        } else if (name == "CarrierStats") {
            const std::string callsign = event.str("Callsign");
            carrier_callsigns_[event.num("CarrierID")] = callsign;
            auto &carrier = s.carrier_or_create();
            carrier.name = event.str("Name");
            carrier.callsign = callsign;
        } else if (name == "CarrierJumpRequest" || name == "CarrierJumpCancelled") {
            if (const auto it = carrier_callsigns_.find(event.num("CarrierID")); it != carrier_callsigns_.end())
                return event.with("Callsign", it->second);
        } else if (name == "CarrierLocation") {
            s.carrier_or_create().system_name = event.str("StarSystem");
        } else if (name == "CarrierBankTransfer") {
            // States the commander's balance outright.
            if (const auto balance = event.opt_long("PlayerBalance")) s.credits = *balance;
        } else if (name == "Loadout") {
            const std::string type = canonicalise(event.str("Ship"));
            if (type.find("fighter") == std::string::npos && type.find("buggy") == std::string::npos) {
                s.ship_id = event.opt_long("ShipID");
                if (auto ident = event.opt_str("ShipIdent")) s.ship_ident = std::move(ident);
                if (std::string ship_name = event.str("ShipName"); !ship_name.empty())
                    s.ship_name = std::move(ship_name);
                if (s.ship_type != type) {
                    s.ship_localised = ship_display_name(type);
                    s.ship_type = type;
                }
                s.hull_value = event.opt_long("HullValue");
                s.modules_value = event.opt_long("ModulesValue");
                s.unladen_mass = event.dec("UnladenMass");
                s.cargo_capacity = event.num("CargoCapacity");
                s.max_jump_range = event.dec("MaxJumpRange");
                const json &fuel = event.object("FuelCapacity");
                s.fuel_capacity = fuel_tanks{field::dec(fuel, "Main"), field::dec(fuel, "Reserve")};
                s.rebuy = event.opt_long("Rebuy");

                // A Loadout is a full snapshot of the fit.
                s.modules.clear();
                for (const auto &row: event.rows("Modules")) {
                    ship_module m = module_from(row);
                    const bool hardpoint = m.slot.find("Hardpoint") != std::string::npos && !m.slot.starts_with(
                                               "TinyHardpoint");
                    if (hardpoint && m.ammo_in_clip == 1 && m.ammo_in_hopper == 1) {
                        m.ammo_in_clip.reset();
                        m.ammo_in_hopper.reset();
                    }
                    s.modules[m.slot] = std::move(m);
                }
            }
        } else if (name == "ModuleBuy") {
            ship_module m;
            m.item = canonicalise(event.str("BuyItem"));
            m.slot = event.str("Slot");
            m.value = event.num("BuyPrice");
            s.modules[m.slot] = std::move(m);
            // The module it replaces can be sold in the same transaction.
            s.credits += event.num("SellPrice") - event.num("BuyPrice");
        } else if (name == "ModuleSell") {
            s.modules.erase(event.str("Slot"));
            s.credits += event.num("SellPrice");
        } else if (name == "ModuleSellRemote") {
            s.credits += event.num("SellPrice");
        } else if (name == "ModuleStore") {
            s.modules.erase(event.str("Slot"));
            s.credits -= event.num("Cost");
        } else if (name == "ModuleRetrieve") {
            s.credits -= event.num("Cost");
        } else if (name == "ModuleSwap") {
            const std::string from = event.str("FromSlot");
            const std::string to = event.str("ToSlot");
            auto from_node = s.modules.extract(from);
            auto to_node = s.modules.extract(to);
            if (from_node) {
                from_node.mapped().slot = to;
                from_node.key() = to;
                s.modules.insert(std::move(from_node));
            }
            if (to_node) {
                to_node.mapped().slot = from;
                to_node.key() = from;
                s.modules.insert(std::move(to_node));
            }
        } else if (name == "Undocked") {
            s.station_name.reset();
            s.market_id.reset();
            s.station_type.reset();
            s.station_services.clear();
            s.is_docked = false;
        } else if (name == "Embark") {
            s.station_name.reset();
            s.market_id.reset();
            if (event.flag("OnStation")) {
                s.station_name = event.opt_str("StationName");
                s.market_id = event.opt_long("MarketID");
            }
            s.on_foot = false;
            s.taxi = event.flag("Taxi");
            s.backpack.clear();
        } else if (name == "Disembark") {
            if (event.flag("OnStation")) {
                s.station_name = event.opt_str("StationName");
                s.market_id = event.opt_long("MarketID");
            } else {
                s.station_name.reset();
                s.market_id.reset();
            }
            s.on_foot = true;
            s.taxi = false;
            s.dropship = false;
        } else if (name == "DropshipDeploy") {
            s.on_foot = true;
            s.taxi = false;
            s.dropship = false;
        } else if (name == "SupercruiseExit") {
            if (event.str("BodyType") == "Station") {
                s.body.reset();
                s.body_id.reset();
                s.body_type.reset();
            }
        } else if (name == "Docked") {
            s.is_docked = true;
            s.station_name = event.str("StationName");
            s.market_id = event.opt_long("MarketID");
            s.station_type = event.opt_str("StationType");
            s.station_services = event.strings("StationServices");
        } else if (name == "Location") {
            // At login, or after a fleet carrier jump while docked on it. Docked at an orbital
            // station, Body is the station and BodyType is "Station".
            s.body = event.str("Body");
            s.body_id = event.opt_long("BodyID");
            s.body_type = event.opt_str("BodyType");
            s.is_docked = event.flag("Docked");
            set_star_pos(event);
            s.system_address = event.opt_long("SystemAddress");
            s.system_population = event.opt_long("Population");
            s.system_name = system_name(event);
            s.station_name = event.opt_str("StationName");
            if (event.str("BodyType") == "Station") s.station_name = event.str("Body");
            s.market_id = event.opt_long("MarketID");
            s.station_type = event.opt_str("StationType");
            s.station_services = event.strings("StationServices");
            s.taxi = event.flag("Taxi");
            if (!s.taxi) s.dropship = false;
        } else if (name == "CarrierJump" || name == "FSDJump") {
            const bool carrier = name == "CarrierJump";
            if (carrier) {
                s.body = event.str("Body");
                s.body_id = event.opt_long("BodyID");
                s.body_type = event.opt_str("BodyType");
            } else {
                s.body.reset();
                s.body_id.reset();
                s.body_type.reset();
            }
            set_star_pos(event);
            s.system_address = event.opt_long("SystemAddress");
            s.system_population = event.opt_long("Population");
            s.system_name = system_name(event);
            s.station_name.reset();
            s.market_id.reset();
            s.station_type.reset();
            s.station_services.clear();
            s.taxi = event.flag("Taxi");
            if (!s.taxi) s.dropship = false;

            // "Some Name X8H-B4Z" -> callsign X8H-B4Z
            if (carrier) {
                const std::string station = event.str("StationName");
                if (const auto space = station.rfind(' '); space != std::string::npos && station.size() - space == 8 &&
                                                           station[space + 4] == '-') {
                    return event.with("Callsign", station.substr(space + 1));
                }
            }
        } else if (name == "ApproachBody") {
            s.body = event.str("Body");
            s.body_id = event.opt_long("BodyID");
            s.body_type = "Planet";
        } else if (name == "LeaveBody") {
            s.body.reset();
            s.body_id.reset();
            s.body_type.reset();
        } else if (name == "SupercruiseEntry") {
            if (s.body_type == "Station") {
                s.body.reset();
                s.body_id.reset();
                s.body_type.reset();
            }
            s.station_name.reset();
            s.market_id.reset();
            s.station_type.reset();
            s.station_services.clear();
        } else if (name == "Music") {
            if (event.str("MusicTrack") == "MainMenu") {
                s.body.reset();
                s.body_id.reset();
                s.body_type.reset();
            }
        } else if (name == "Rank" || name == "Promotion") {
            for (const auto &[key, value]: event.payload().items()) {
                if (value.is_number_integer()) s.rank[key] = rank_level{value.get<int>(), 0};
            }
        } else if (name == "Progress") {
            for (const auto &[key, value]: event.payload().items()) {
                const auto it = s.rank.find(key);
                if (it != s.rank.end() && value.is_number()) it->second.progress = std::min(value.get<int>(), 100);
            }
        } else if (name == "Reputation") {
            for (const auto &[key, value]: event.payload().items()) {
                if (value.is_number()) s.reputation[key] = value.get<double>();
            }
        } else if (name == "EngineerProgress") {
            if (const json &engineers = event.rows("Engineers"); !engineers.empty()) {
                s.engineers.clear(); // the list form is a full snapshot
                for (const auto &row: engineers) {
                    if (const std::string engineer = field::str(row, "Engineer"); !engineer.empty())
                        merge_engineer(s.engineers[engineer], row);
                }
            } else if (const std::string engineer = event.str("Engineer"); !engineer.empty()) {
                merge_engineer(s.engineers[engineer], event.payload());
            }
        } else if (name == "Cargo") {
            if (event.str("Vessel", "Ship") == "Ship") {
                if (event.payload().contains("Inventory")) {
                    s.cargo.clear(); // an authoritative snapshot
                    for (const auto &row: event.rows("Inventory"))
                        s.cargo[canonicalise(field::str(row, "Name"))] = count(row, "Count");
                } else if (p == phase::live) {
                    return delegate_to(event);
                }
            }
        } else if (name == "CargoTransfer") {
            for (const auto &row: event.rows("Transfers")) {
                const bool to_ship = lower(field::str(row, "Direction")) == "toship";
                const int n = count(row, "Count");
                adjust(s.cargo, canonicalise(field::str(row, "Type")), to_ship ? n : -n);
            }
        } else if (name == "BackpackChange") {
            for (const auto &row: event.rows("Added")) apply_backpack_change(row, 1);
            for (const auto &row: event.rows("Removed")) apply_backpack_change(row, -1);
        } else if (name == "BuyMicroResources" || name == "CarrierBuy") {
            s.credits -= event.num("Price");
        } else if (name == "SellMicroResources") {
            s.credits += event.num("Price");
        } else if (name == "BookDropship") {
            s.credits -= event.num("Cost");
            s.dropship = true;
        } else if (name == "BookTaxi" || name == "Resurrect" || name == "PowerplayFastTrack" ||
                   name == "RestockVehicle" || name == "Repair" || name == "RepairAll" ||
                   name == "RefuelAll" || name == "RefuelPartial" || name == "CrewHire" ||
                   name == "BuyAmmo" || name == "BuyTradeData" || name == "BuyExplorationData") {
            s.credits -= event.num("Cost");
        } else if (name == "CancelDropship") {
            s.credits += event.num("Refund");
            s.dropship = false;
            s.taxi = false;
        } else if (name == "CancelTaxi") {
            s.credits += event.num("Refund");
            s.taxi = false;
        } else if (name == "Market" || name == "Shipyard" || name == "NavRoute" || name == "Outfitting" ||
                   name == "FCMaterials" || name == "ModuleInfo") {
            if (p == phase::live) return delegate_to(event);
        } else if (name == "CollectCargo" || name == "MiningRefined") {
            adjust(s.cargo, canonicalise(event.str("Type")), 1);
        } else if (name == "MarketBuy" || name == "BuyDrones") {
            adjust(s.cargo, canonicalise(event.str("Type")), static_cast<int>(event.num("Count")));
            s.credits -= event.num("TotalCost");
        } else if (name == "EjectCargo") {
            adjust(s.cargo, canonicalise(event.str("Type")), -static_cast<int>(event.num("Count")));
        } else if (name == "MarketSell" || name == "SellDrones") {
            adjust(s.cargo, canonicalise(event.str("Type")), -static_cast<int>(event.num("Count")));
            s.credits += event.num("TotalSale");
        } else if (name == "SearchAndRescue") {
            adjust(s.cargo, canonicalise(event.str("Name")), -static_cast<int>(event.num("Count")));
        } else if (name == "Materials") {
            s.raw.clear(); // an authoritative snapshot
            s.manufactured.clear();
            s.encoded.clear();
            auto load = [](const json &rows, std::map<std::string, int> &target) {
                for (const auto &row: rows) target[canonicalise(field::str(row, "Name"))] = count(row, "Count");
            };
            load(event.rows("Raw"), s.raw);
            load(event.rows("Manufactured"), s.manufactured);
            load(event.rows("Encoded"), s.encoded);
        } else if (name == "MaterialCollected") {
            if (auto *map = material_map(event.str("Category")))
                adjust(*map, canonicalise(event.str("Name")), static_cast<int>(event.num("Count")));
        } else if (name == "MaterialDiscarded" || name == "ScientificResearch") {
            if (auto *map = material_map(event.str("Category")))
                adjust(*map, canonicalise(event.str("Name")), -static_cast<int>(event.num("Count")));
        } else if (name == "Synthesis") {
            for (const auto &row: event.rows("Materials"))
                consume_material(canonicalise(field::str(row, "Name")), count(row, "Count"));
        } else if (name == "MaterialTrade") {
            const json &paid = event.object("Paid");
            if (auto *map = material_map(category(field::str(paid, "Category"))))
                adjust_existing(*map, canonicalise(field::str(paid, "Material")), -count(paid, "Quantity"));
            const json &received = event.object("Received");
            if (auto *map = material_map(category(field::str(received, "Category"))))
                adjust(*map, canonicalise(field::str(received, "Material")), count(received, "Quantity"));
        } else if (name == "EngineerCraft") {
            for (const auto &row: event.rows("Ingredients"))
                consume_material(canonicalise(field::str(row, "Name")), count(row, "Count"));

            const auto it = s.modules.find(event.str("Slot"));
            if (it != s.modules.end()) {
                if (it->second.item != canonicalise(event.str("Module"))) {
                    spdlog::warn("EngineerCraft names a module that is not in slot {}", event.str("Slot"));
                } else {
                    module_engineering e;
                    e.blueprint_id = event.num("BlueprintID");
                    e.blueprint_name = event.str("BlueprintName");
                    e.engineer = event.opt_str("Engineer");
                    e.engineer_id = event.num("EngineerID");
                    e.experimental_effect = event.opt_str("ExperimentalEffect");
                    e.experimental_effect_localised = event.opt_str("ExperimentalEffect_Localised");
                    e.level = event.num("Level");
                    e.quality = event.dec("Quality");
                    for (const auto &row: event.rows("Modifiers")) e.modifiers.push_back(modifier_from(row));
                    it->second.engineering = std::move(e);
                }
            }
        } else if (name == "MissionCompleted") {
            s.credits += event.num("Reward") - event.num("Donated");
            for (const auto &row: event.rows("CommodityReward"))
                adjust(s.cargo, canonicalise(field::str(row, "Name")), count(row, "Count"));
            for (const auto &row: event.rows("MaterialsReward")) {
                if (auto *map = material_map(category(field::str(row, "Category"))))
                    adjust(*map, canonicalise(field::str(row, "Name")), count(row, "Count"));
            }
        } else if (name == "EngineerContribution") {
            const int quantity = static_cast<int>(event.num("Quantity"));
            if (const auto commodity = event.opt_str("Commodity"))
                adjust_existing(s.cargo, canonicalise(*commodity), -quantity);
            if (const auto material = event.opt_str("Material")) consume_material(canonicalise(*material), quantity);
        } else if (name == "TechnologyBroker") {
            for (const auto &row: event.rows("Commodities"))
                adjust_existing(s.cargo, canonicalise(field::str(row, "Name")), -count(row, "Count"));
            for (const auto &row: event.rows("Materials")) {
                if (auto *map = material_map(category(field::str(row, "Category"))))
                    adjust_existing(*map, canonicalise(field::str(row, "Name")), -count(row, "Count"));
            }
        } else if (name == "JoinACrew") {
            s.captain = event.str("Captain");
            s.role = "Idle";
            clear_location();
            s.on_foot = false;
        } else if (name == "ChangeCrewRole") {
            s.role = event.str("Role");
        } else if (name == "QuitACrew") {
            s.captain.reset();
            s.role.reset();
            clear_location();
        } else if (name == "Friends") {
            const std::string status = event.str("Status");
            if (status == "Online" || status == "Added") {
                s.friends.insert(event.str("Name"));
            } else {
                s.friends.erase(event.str("Name"));
            }
        } else if (name == "SellExplorationData" || name == "MultiSellExplorationData") {
            s.credits += event.num("TotalEarnings");
        } else if (name == "SellOrganicData") {
            for (const auto &row: event.rows("BioData"))
                s.credits += field::num(row, "Value") + field::num(row, "Bonus");
        } else if (name == "FetchRemoteModule") {
            s.credits -= event.num("TransferCost");
        } else if (name == "CommunityGoalReward") {
            s.credits += event.num("Reward");
        } else if (name == "RedeemVoucher" || name == "PowerplaySalary") {
            s.credits += event.num("Amount");
        } else if (name == "SellShipOnRebuy" || name == "ShipyardSell") {
            s.credits += event.num("ShipPrice");
        } else if (name == "ShipyardTransfer") {
            s.credits -= event.num("TransferPrice");
        } else if (name == "PayBounties" || name == "PayFines" || name == "PayLegacyFines") {
            s.credits -= event.num("Amount");
        } else if (name == "Powerplay") {
            s.powerplay.power = event.str("Power");
            s.powerplay.rank = static_cast<int>(event.num("Rank"));
            s.powerplay.merits = event.num("Merits");
            s.powerplay.votes = event.opt_long("Votes");
            s.powerplay.time_pledged = event.num("TimePledged");
        }

        return event;
    }
} // namespace journal
