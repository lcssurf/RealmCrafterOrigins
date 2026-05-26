#include "ability_json_validator.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <set>
#include <string>

namespace rco::gue {

using json = nlohmann::json;

namespace {

const std::set<std::string> kValidStatsScaling = {"STR", "DEX", "INT", "WIS", "PER", "LEVEL"};
const std::set<std::string> kValidStatsCrit = {"STR", "DEX", "INT", "WIS", "PER"};

std::string ToUpper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string TrimWhitespace(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\n\r");
    return value.substr(first, last - first + 1);
}

} // namespace

ValidationResult ValidateDamageStatScaleJSON(const std::string& json_str) {
    ValidationResult result;
    const std::string trimmed = TrimWhitespace(json_str);
    if (trimmed.empty() || trimmed == "{}") {
        return result;
    }

    json j;
    try {
        j = json::parse(trimmed);
    } catch (const json::parse_error& e) {
        result.syntactically_valid = false;
        result.semantically_valid = false;
        result.error_message = std::string("JSON parse error: ") + e.what();
        return result;
    }

    if (!j.contains("scaling")) {
        result.semantically_valid = false;
        result.error_message = "Missing 'scaling' array";
        return result;
    }
    if (!j["scaling"].is_array()) {
        result.semantically_valid = false;
        result.error_message = "'scaling' must be an array";
        return result;
    }

    for (std::size_t i = 0; i < j["scaling"].size(); ++i) {
        const auto& item = j["scaling"][i];
        if (!item.is_object()) {
            result.semantically_valid = false;
            result.error_message = "scaling[" + std::to_string(i) + "] must be an object";
            return result;
        }
        if (!item.contains("stat") || !item["stat"].is_string()) {
            result.semantically_valid = false;
            result.error_message = "scaling[" + std::to_string(i) + "] missing 'stat' (string)";
            return result;
        }
        const std::string stat = ToUpper(item["stat"].get<std::string>());
        if (kValidStatsScaling.count(stat) == 0) {
            result.semantically_valid = false;
            result.error_message = "scaling[" + std::to_string(i) +
                "] stat '" + stat + "' invalid (use STR/DEX/INT/WIS/PER/LEVEL)";
            return result;
        }
        if (!item.contains("coef") || !item["coef"].is_number()) {
            result.semantically_valid = false;
            result.error_message = "scaling[" + std::to_string(i) + "] missing 'coef' (number)";
            return result;
        }
        const double coef = item["coef"].get<double>();
        if (coef < -10.0 || coef > 10.0) {
            result.semantically_valid = false;
            result.error_message = "scaling[" + std::to_string(i) +
                "] coef " + std::to_string(coef) + " out of range (-10.0 to 10.0)";
            return result;
        }
    }

    return result;
}

ValidationResult ValidateCritPolicyJSON(const std::string& json_str) {
    ValidationResult result;
    const std::string trimmed = TrimWhitespace(json_str);
    if (trimmed.empty() || trimmed == "{}") {
        return result;
    }

    json j;
    try {
        j = json::parse(trimmed);
    } catch (const json::parse_error& e) {
        result.syntactically_valid = false;
        result.semantically_valid = false;
        result.error_message = std::string("JSON parse error: ") + e.what();
        return result;
    }

    const char* required[] = {
        "base_chance_pct",
        "scaling_stat",
        "scaling_softcap_value",
        "scaling_softcap_pct",
        "damage_multiplier",
    };
    for (const char* field : required) {
        if (!j.contains(field)) {
            result.semantically_valid = false;
            result.error_message = std::string("Missing required field: ") + field;
            return result;
        }
    }

    if (!j["base_chance_pct"].is_number()) {
        result.semantically_valid = false;
        result.error_message = "base_chance_pct must be number";
        return result;
    }
    const double base_chance_pct = j["base_chance_pct"].get<double>();
    if (base_chance_pct < 0.0 || base_chance_pct > 100.0) {
        result.semantically_valid = false;
        result.error_message = "base_chance_pct out of range (0.0-100.0): " + std::to_string(base_chance_pct);
        return result;
    }

    if (!j["scaling_stat"].is_string()) {
        result.semantically_valid = false;
        result.error_message = "scaling_stat must be string";
        return result;
    }
    const std::string scaling_stat = ToUpper(j["scaling_stat"].get<std::string>());
    if (kValidStatsCrit.count(scaling_stat) == 0) {
        result.semantically_valid = false;
        result.error_message = "scaling_stat '" + scaling_stat + "' invalid (use STR/DEX/INT/WIS/PER)";
        return result;
    }

    if (!j["scaling_softcap_value"].is_number_integer()) {
        result.semantically_valid = false;
        result.error_message = "scaling_softcap_value must be integer";
        return result;
    }
    const int scaling_softcap_value = j["scaling_softcap_value"].get<int>();
    if (scaling_softcap_value <= 0) {
        result.semantically_valid = false;
        result.error_message = "scaling_softcap_value must be > 0";
        return result;
    }

    if (!j["scaling_softcap_pct"].is_number()) {
        result.semantically_valid = false;
        result.error_message = "scaling_softcap_pct must be number";
        return result;
    }
    const double scaling_softcap_pct = j["scaling_softcap_pct"].get<double>();
    if (scaling_softcap_pct < 0.0 || scaling_softcap_pct > 1.0) {
        result.semantically_valid = false;
        result.error_message = "scaling_softcap_pct out of range (0.0-1.0): " + std::to_string(scaling_softcap_pct);
        return result;
    }

    if (!j["damage_multiplier"].is_number()) {
        result.semantically_valid = false;
        result.error_message = "damage_multiplier must be number";
        return result;
    }
    const double damage_multiplier = j["damage_multiplier"].get<double>();
    if (damage_multiplier < 1.0) {
        result.semantically_valid = false;
        result.error_message = "damage_multiplier must be >= 1.0: " + std::to_string(damage_multiplier);
        return result;
    }

    return result;
}

} // namespace rco::gue
