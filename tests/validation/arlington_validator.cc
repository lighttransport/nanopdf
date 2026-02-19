#include "arlington_validator.hh"
#include <iostream>
#include <sstream>

namespace nanopdf {
namespace arlington {

// ============================================================================
// Severity
// ============================================================================

const char* severity_to_string(Severity sev) {
    switch (sev) {
        case Severity::Info:    return "INFO";
        case Severity::Warning: return "WARN";
        case Severity::Error:   return "ERROR";
    }
    return "UNKNOWN";
}

// ============================================================================
// ValidationResult
// ============================================================================

void ValidationResult::add(Severity sev, const std::string& path,
                           const std::string& key, const std::string& message,
                           const std::string& ref) {
    ValidationFinding finding;
    finding.severity = sev;
    finding.object_path = path;
    finding.key = key;
    finding.message = message;
    finding.arlington_ref = ref;
    findings.push_back(std::move(finding));

    switch (sev) {
        case Severity::Info:    info_count++; break;
        case Severity::Warning: warning_count++; break;
        case Severity::Error:   error_count++; success = false; break;
    }
}

void ValidationResult::print_summary() const {
    std::cout << "\n[validation] Summary:\n";
    std::cout << "  Errors:   " << error_count << "\n";
    std::cout << "  Warnings: " << warning_count << "\n";
    std::cout << "  Info:     " << info_count << "\n";
    std::cout << "  Status:   " << (success ? "PASS" : "FAIL") << "\n";
}

void ValidationResult::print_findings(Severity min_severity) const {
    for (const auto& f : findings) {
        if (f.severity < min_severity) continue;
        std::cout << "  [" << severity_to_string(f.severity) << "] "
                  << f.object_path;
        if (!f.key.empty()) std::cout << "." << f.key;
        std::cout << ": " << f.message;
        if (!f.arlington_ref.empty()) std::cout << " (ref: " << f.arlington_ref << ")";
        std::cout << "\n";
    }
}

// ============================================================================
// Validator
// ============================================================================

Validator::Validator(const ArlingtonModel& model) : model_(model) {}

ArlingtonType Validator::value_type_to_arlington(Value::Type vtype) {
    switch (vtype) {
        case Value::BOOLEAN:    return ArlingtonType::Boolean;
        case Value::NUMBER:     return ArlingtonType::Number;
        case Value::STRING:     return ArlingtonType::String;
        case Value::NAME:       return ArlingtonType::Name;
        case Value::ARRAY:      return ArlingtonType::Array;
        case Value::DICTIONARY: return ArlingtonType::Dictionary;
        case Value::STREAM:     return ArlingtonType::Stream;
        case Value::NULL_OBJ:   return ArlingtonType::Null;
        default:                return ArlingtonType::Unknown;
    }
}

bool Validator::type_matches(const Value& val,
                             const std::vector<ArlingtonType>& allowed) {
    ArlingtonType vtype = value_type_to_arlington(val.type);

    for (auto at : allowed) {
        if (at == vtype) return true;
        // Integer is a subtype of Number
        if (at == ArlingtonType::Integer && vtype == ArlingtonType::Number) {
            double n = val.number;
            if (n == static_cast<int>(n)) return true;
        }
        if (at == ArlingtonType::Number && vtype == ArlingtonType::Number) return true;
        // Bitmask maps to integer
        if (at == ArlingtonType::Bitmask && vtype == ArlingtonType::Number) {
            double n = val.number;
            if (n == static_cast<int>(n)) return true;
        }
        // Rectangle and Matrix are arrays
        if ((at == ArlingtonType::Rectangle || at == ArlingtonType::Matrix) &&
            vtype == ArlingtonType::Array) return true;
        // Date is a string
        if (at == ArlingtonType::Date && vtype == ArlingtonType::String) return true;
        // NameTree/NumberTree are dictionaries
        if ((at == ArlingtonType::NameTree || at == ArlingtonType::NumberTree) &&
            vtype == ArlingtonType::Dictionary) return true;
    }
    return false;
}

Value Validator::resolve(const Pdf& pdf, const Value& val) const {
    if (val.type == Value::REFERENCE) {
        auto resolved = resolve_reference(pdf, val.ref_object_number,
                                          val.ref_generation_number);
        if (resolved.success) return std::move(resolved.value);
    }
    return val;
}

// ============================================================================
// Core Recursive Walker
// ============================================================================

void Validator::validate_object(const Pdf& pdf, const Value& val,
                                 const std::string& def_name,
                                 const std::string& path, int depth) {
    if (depth > kMaxDepth) {
        result_.add(Severity::Warning, path, "", "Max depth reached", def_name);
        return;
    }

    // Resolve references with cycle detection
    const Value* resolved = &val;
    Value resolved_storage;

    if (val.type == Value::REFERENCE) {
        if (visited_.count(val.ref_object_number)) return;  // Already visited
        visited_.insert(val.ref_object_number);
        auto ref_result = resolve_reference(pdf, val.ref_object_number,
                                             val.ref_generation_number);
        if (!ref_result.success) {
            result_.add(Severity::Warning, path, "",
                        "Could not resolve reference to obj " +
                        std::to_string(val.ref_object_number), def_name);
            return;
        }
        resolved_storage = std::move(ref_result.value);
        resolved = &resolved_storage;
    }

    // Look up definition
    const ObjectDefinition* obj_def = model_.get_object_def(def_name);
    if (!obj_def) {
        result_.add(Severity::Info, path, "",
                    "Definition '" + def_name + "' not found in model", "");
        return;
    }

    if (resolved->type == Value::DICTIONARY || resolved->type == Value::STREAM) {
        const Dictionary& dict = (resolved->type == Value::STREAM) ?
            resolved->stream.dict : resolved->dict;
        validate_dict_keys(pdf, dict, *obj_def, path, depth);
    } else if (resolved->type == Value::ARRAY) {
        validate_array_elements(pdf, resolved->array, *obj_def, path, depth);
    }
}

void Validator::validate_dict_keys(const Pdf& pdf, const Dictionary& dict,
                                    const ObjectDefinition& obj_def,
                                    const std::string& path, int depth) {
    // Check required keys
    for (const auto& key_def : obj_def.keys) {
        if (key_def.key == "*") continue;

        // Skip keys introduced after our PDF version
        if (!key_def.since_version.empty() &&
            key_def.since_version.find("fn:") == std::string::npos) {
            PdfVersion since_ver = PdfVersion::parse(key_def.since_version);
            if (version_ < since_ver) continue;
        }

        // Skip deprecated keys
        if (!key_def.deprecated_in.empty()) {
            PdfVersion dep_ver = PdfVersion::parse(key_def.deprecated_in);
            if (version_ >= dep_ver) continue;
        }

        bool required = evaluate_required(key_def.required_expr, dict);
        if (required && dict.find(key_def.key) == dict.end()) {
            result_.add(Severity::Error, path, key_def.key,
                        "Required key missing", obj_def.name);
        }
    }

    // Validate each key present in the dictionary
    for (const auto& kv : dict) {
        const std::string& key = kv.first;
        const Value& val = kv.second;

        const KeyDefinition* key_def = obj_def.get_key(key);
        if (!key_def) key_def = obj_def.get_key("*");
        if (!key_def) {
            result_.add(Severity::Info, path, key,
                        "Key not defined in Arlington model", obj_def.name);
            continue;
        }

        // Resolve references before type checking
        Value resolved_val = resolve(pdf, val);

        // Type check
        if (!key_def->types.empty() && resolved_val.type != Value::UNDEFINED) {
            if (!type_matches(resolved_val, key_def->types)) {
                std::string msg = "Type mismatch: got ";
                msg += type_to_string(value_type_to_arlington(resolved_val.type));
                msg += ", expected ";
                for (size_t i = 0; i < key_def->types.size(); ++i) {
                    if (i > 0) msg += "|";
                    msg += type_to_string(key_def->types[i]);
                }
                result_.add(Severity::Error, path, key, msg, obj_def.name);
            }
        }

        // Check possible values
        check_possible_values(resolved_val, key_def->possible_values,
                              path, key, obj_def.name);

        // Check indirect reference requirements
        check_indirect_reference(val, key_def->indirect_reference,
                                  path, key, obj_def.name);

        // Follow links recursively
        follow_links(pdf, val, *key_def, path + "." + key, depth + 1);
    }
}

void Validator::validate_array_elements(const Pdf& pdf,
                                         const std::vector<Value>& arr,
                                         const ObjectDefinition& obj_def,
                                         const std::string& path, int depth) {
    const KeyDefinition* wildcard = obj_def.get_key("*");

    for (size_t i = 0; i < arr.size(); ++i) {
        // Try specific index first, then wildcard
        std::string idx_str = std::to_string(i);
        const KeyDefinition* key_def = obj_def.get_key(idx_str);
        if (!key_def) key_def = wildcard;
        if (!key_def) continue;

        const Value& elem = arr[i];
        Value resolved = resolve(pdf, elem);

        // Type check
        if (!key_def->types.empty() && resolved.type != Value::UNDEFINED) {
            if (!type_matches(resolved, key_def->types)) {
                std::string msg = "Type mismatch: got ";
                msg += type_to_string(value_type_to_arlington(resolved.type));
                msg += ", expected ";
                for (size_t j = 0; j < key_def->types.size(); ++j) {
                    if (j > 0) msg += "|";
                    msg += type_to_string(key_def->types[j]);
                }
                result_.add(Severity::Error, path, idx_str, msg, obj_def.name);
            }
        }

        // Check possible values
        check_possible_values(resolved, key_def->possible_values,
                              path, idx_str, obj_def.name);

        // Follow links
        follow_links(pdf, elem, *key_def,
                      path + "[" + idx_str + "]", depth + 1);
    }
}

// ============================================================================
// Link Following
// ============================================================================

void Validator::follow_links(const Pdf& pdf, const Value& val,
                              const KeyDefinition& key_def,
                              const std::string& path, int depth) {
    if (key_def.link_groups.empty()) return;
    if (depth > kMaxDepth) return;

    // Resolve to determine type for link group selection
    const Value* check_val = &val;
    Value resolved_storage;
    if (val.type == Value::REFERENCE) {
        resolved_storage = resolve(pdf, val);
        check_val = &resolved_storage;
    }

    // Only follow links for dict, stream, or array
    if (check_val->type != Value::DICTIONARY &&
        check_val->type != Value::STREAM &&
        check_val->type != Value::ARRAY) return;

    // Determine which link group based on type index
    int type_idx = find_type_index(*check_val, key_def.types);
    if (type_idx < 0) type_idx = 0;

    size_t group_idx = static_cast<size_t>(type_idx);
    if (group_idx >= key_def.link_groups.size()) group_idx = 0;
    if (group_idx >= key_def.link_groups.size()) return;

    const LinkGroup& group = key_def.link_groups[group_idx];
    if (group.alternatives.empty()) return;

    if (check_val->type == Value::DICTIONARY || check_val->type == Value::STREAM) {
        const Dictionary& dict = (check_val->type == Value::STREAM) ?
            check_val->stream.dict : check_val->dict;

        std::string def_name;
        if (group.alternatives.size() == 1) {
            def_name = extract_def_name(group.alternatives[0]);
        } else {
            def_name = choose_definition(pdf, dict, group.alternatives);
        }

        if (!def_name.empty()) {
            validate_object(pdf, val, def_name, path, depth);
        }
    } else if (check_val->type == Value::ARRAY) {
        std::string def_name;
        if (!group.alternatives.empty()) {
            def_name = extract_def_name(group.alternatives[0]);
        }
        if (!def_name.empty()) {
            validate_object(pdf, val, def_name, path, depth);
        }
    }
}

std::string Validator::choose_definition(
        const Pdf& pdf, const Dictionary& dict,
        const std::vector<std::string>& alternatives) {
    // Extract bare definition names
    std::vector<std::string> def_names;
    for (const auto& alt : alternatives) {
        std::string name = extract_def_name(alt);
        if (!name.empty()) def_names.push_back(name);
    }

    if (def_names.empty()) return "";
    if (def_names.size() == 1) return def_names[0];

    // Extract discriminator keys from the dictionary
    std::string type_val, subtype_val, s_val;
    auto it = dict.find("Type");
    if (it != dict.end() && it->second.type == Value::NAME)
        type_val = it->second.name;
    it = dict.find("Subtype");
    if (it != dict.end() && it->second.type == Value::NAME)
        subtype_val = it->second.name;
    it = dict.find("S");
    if (it != dict.end() && it->second.type == Value::NAME)
        s_val = it->second.name;

    int best_score = -1;
    std::string best_name;

    for (const auto& name : def_names) {
        const ObjectDefinition* obj_def = model_.get_object_def(name);
        if (!obj_def) continue;

        int score = 0;

        // Match by Type possible values
        if (!type_val.empty()) {
            const KeyDefinition* type_key = obj_def->get_key("Type");
            if (type_key) {
                for (const auto& pv : type_key->possible_values) {
                    if (pv == type_val) { score += 10; break; }
                }
            }
        }

        // Match by Subtype possible values
        if (!subtype_val.empty()) {
            const KeyDefinition* subtype_key = obj_def->get_key("Subtype");
            if (subtype_key) {
                for (const auto& pv : subtype_key->possible_values) {
                    if (pv == subtype_val) { score += 10; break; }
                }
            }
        }

        // Match by S possible values
        if (!s_val.empty()) {
            const KeyDefinition* s_key = obj_def->get_key("S");
            if (s_key) {
                for (const auto& pv : s_key->possible_values) {
                    if (pv == s_val) { score += 10; break; }
                }
            }
        }

        // Score by key overlap
        for (const auto& kv : dict) {
            if (obj_def->get_key(kv.first)) score++;
        }

        if (score > best_score) {
            best_score = score;
            best_name = name;
        }
    }

    return best_name.empty() ? def_names[0] : best_name;
}

std::string Validator::extract_def_name(const std::string& link_entry) {
    if (link_entry.empty()) return "";

    // fn:SinceVersion(1.6,ActionGoToE) → ActionGoToE
    // fn:IsPDFVersion(1.2,ActionNOP) → ActionNOP
    if (link_entry.find("fn:SinceVersion(") == 0 ||
        link_entry.find("fn:IsPDFVersion(") == 0) {
        // Find the last comma at paren depth 1, extract what's after it
        int paren_depth = 0;
        size_t last_comma = std::string::npos;
        for (size_t i = 0; i < link_entry.size(); ++i) {
            if (link_entry[i] == '(') paren_depth++;
            else if (link_entry[i] == ')') paren_depth--;
            else if (link_entry[i] == ',' && paren_depth == 1) {
                last_comma = i;
            }
        }
        if (last_comma != std::string::npos) {
            std::string name = link_entry.substr(last_comma + 1);
            while (!name.empty() && name.back() == ')') name.pop_back();
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            while (!name.empty() && name.back() == ' ') name.pop_back();
            return name;
        }
    }

    // fn:Extension(ExtName,DefName) → DefName
    if (link_entry.find("fn:Extension(") == 0) {
        int paren_depth = 0;
        size_t last_comma = std::string::npos;
        for (size_t i = 0; i < link_entry.size(); ++i) {
            if (link_entry[i] == '(') paren_depth++;
            else if (link_entry[i] == ')') paren_depth--;
            else if (link_entry[i] == ',' && paren_depth == 1) {
                last_comma = i;
            }
        }
        if (last_comma != std::string::npos) {
            std::string name = link_entry.substr(last_comma + 1);
            while (!name.empty() && name.back() == ')') name.pop_back();
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            while (!name.empty() && name.back() == ' ') name.pop_back();
            return name;
        }
        return "";  // fn:Extension with single arg → skip
    }

    // Other fn: expressions → skip
    if (link_entry.find("fn:") == 0) return "";

    return link_entry;
}

int Validator::find_type_index(const Value& val,
                                const std::vector<ArlingtonType>& types) {
    ArlingtonType vtype = value_type_to_arlington(val.type);

    for (size_t i = 0; i < types.size(); ++i) {
        if (types[i] == vtype) return static_cast<int>(i);
        // Integer is subtype of Number
        if (types[i] == ArlingtonType::Integer && vtype == ArlingtonType::Number) {
            double n = val.number;
            if (n == static_cast<int>(n)) return static_cast<int>(i);
        }
        // Bitmask maps to integer
        if (types[i] == ArlingtonType::Bitmask && vtype == ArlingtonType::Number) {
            double n = val.number;
            if (n == static_cast<int>(n)) return static_cast<int>(i);
        }
        // Rectangle/Matrix are arrays
        if ((types[i] == ArlingtonType::Rectangle || types[i] == ArlingtonType::Matrix) &&
            vtype == ArlingtonType::Array) return static_cast<int>(i);
        // Date is string
        if (types[i] == ArlingtonType::Date && vtype == ArlingtonType::String)
            return static_cast<int>(i);
        // NameTree/NumberTree are dictionaries
        if ((types[i] == ArlingtonType::NameTree || types[i] == ArlingtonType::NumberTree) &&
            vtype == ArlingtonType::Dictionary) return static_cast<int>(i);
    }
    return -1;
}

// ============================================================================
// Validation Helpers
// ============================================================================

bool Validator::evaluate_required(const std::string& expr,
                                   const Dictionary& context_dict) {
    if (expr.empty() || expr == "FALSE" || expr == "false" || expr == "0")
        return false;
    if (expr == "TRUE" || expr == "true" || expr == "1")
        return true;

    // fn:IsRequired(fn:IsPresent(KEY))
    const std::string pat1 = "fn:IsRequired(fn:IsPresent(";
    if (expr.find(pat1) == 0) {
        size_t start = pat1.size();
        size_t end = expr.find(')', start);
        if (end != std::string::npos) {
            std::string key = expr.substr(start, end - start);
            return context_dict.find(key) != context_dict.end();
        }
    }

    // fn:IsRequired(@KEY!=VALUE) or fn:IsRequired(@KEY==VALUE)
    const std::string pat2 = "fn:IsRequired(@";
    if (expr.find(pat2) == 0) {
        size_t start = pat2.size();
        size_t neq = expr.find("!=", start);
        size_t eq = expr.find("==", start);

        if (neq != std::string::npos && (eq == std::string::npos || neq < eq)) {
            std::string key = expr.substr(start, neq - start);
            size_t val_start = neq + 2;
            size_t val_end = expr.find(')', val_start);
            if (val_end != std::string::npos) {
                std::string expected = expr.substr(val_start, val_end - val_start);
                auto it = context_dict.find(key);
                if (it == context_dict.end()) return false;
                if (it->second.type == Value::NAME)
                    return it->second.name != expected;
            }
        } else if (eq != std::string::npos) {
            std::string key = expr.substr(start, eq - start);
            size_t val_start = eq + 2;
            size_t val_end = expr.find(')', val_start);
            if (val_end != std::string::npos) {
                std::string expected = expr.substr(val_start, val_end - val_start);
                auto it = context_dict.find(key);
                if (it == context_dict.end()) return false;
                if (it->second.type == Value::NAME)
                    return it->second.name == expected;
            }
        }
    }

    // Complex expressions → conservatively not required
    return false;
}

void Validator::check_indirect_reference(const Value& original_val,
                                          const std::string& expr,
                                          const std::string& path,
                                          const std::string& key,
                                          const std::string& def_name) {
    if (expr.empty() || expr == "FALSE" || expr == "false") return;

    bool is_ref = (original_val.type == Value::REFERENCE);

    if (expr == "TRUE" || expr == "true") {
        if (!is_ref) {
            result_.add(Severity::Warning, path, key,
                        "Must be an indirect reference", def_name);
        }
        return;
    }

    // fn:MustBeDirect() — unconditional
    if (expr == "fn:MustBeDirect()") {
        if (is_ref) {
            result_.add(Severity::Warning, path, key,
                        "Must be a direct object", def_name);
        }
        return;
    }

    // fn:MustBeIndirect() — unconditional
    if (expr == "fn:MustBeIndirect()") {
        if (!is_ref) {
            result_.add(Severity::Warning, path, key,
                        "Must be an indirect reference", def_name);
        }
        return;
    }

    // Conditional or positional expressions → skip
}

void Validator::check_possible_values(const Value& val,
                                       const std::vector<std::string>& possible_values,
                                       const std::string& path,
                                       const std::string& key,
                                       const std::string& def_name) {
    if (possible_values.empty()) return;

    // Filter out fn: expressions
    std::vector<std::string> concrete_values;
    for (const auto& pv : possible_values) {
        if (pv.find("fn:") == std::string::npos) {
            concrete_values.push_back(pv);
        }
    }
    if (concrete_values.empty()) return;

    // Check Name values
    if (val.type == Value::NAME) {
        bool found = false;
        for (const auto& cv : concrete_values) {
            if (cv == val.name) { found = true; break; }
        }
        if (!found) {
            result_.add(Severity::Warning, path, key,
                        "Value '" + val.name + "' not in allowed values",
                        def_name);
        }
    }

    // Check String values
    if (val.type == Value::STRING) {
        bool found = false;
        for (const auto& cv : concrete_values) {
            if (cv == val.str) { found = true; break; }
        }
        if (!found) {
            result_.add(Severity::Warning, path, key,
                        "String value not in allowed values", def_name);
        }
    }
}

// ============================================================================
// Entry Point
// ============================================================================

ValidationResult Validator::validate_document(const Pdf& pdf) {
    result_ = ValidationResult();
    visited_.clear();
    version_ = PdfVersion(pdf.version_major, pdf.version_minor);

    result_.add(Severity::Info, "", "",
                "Validating PDF " + std::to_string(version_.major) + "." +
                std::to_string(version_.minor), "");

    // Wrap trailer dictionary as a Value and validate recursively
    Value trailer_val;
    trailer_val.type = Value::DICTIONARY;
    trailer_val.dict = pdf.trailer;

    validate_object(pdf, trailer_val, "FileTrailer", "Trailer", 0);

    return result_;
}

}  // namespace arlington
}  // namespace nanopdf
