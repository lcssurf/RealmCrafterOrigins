#pragma once

#include <string>

namespace rco::gue {

struct ValidationResult {
    bool syntactically_valid = true;
    bool semantically_valid = true;
    std::string error_message;
};

ValidationResult ValidateDamageStatScaleJSON(const std::string& json_str);
ValidationResult ValidateCritPolicyJSON(const std::string& json_str);

} // namespace rco::gue
