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
        if (resolved.success) return resolved.value;
    }
    return val;
}

void Validator::validate_dict(const Pdf& pdf, const Dictionary& dict,
                              const ObjectDefinition& obj_def,
                              const std::string& path, int depth) {
    if (depth > kMaxDepth) {
        result_.add(Severity::Warning, path, "", "Max recursion depth reached",
                    obj_def.name);
        return;
    }

    // Check required keys
    PdfVersion since_ver;
    for (const auto& key_def : obj_def.keys) {
        if (!key_def.required) continue;

        // Skip keys introduced after our PDF version
        if (!key_def.since_version.empty()) {
            since_ver = PdfVersion::parse(key_def.since_version);
            if (version_ < since_ver) continue;
        }

        // Skip deprecated keys
        if (!key_def.deprecated_in.empty()) {
            PdfVersion dep_ver = PdfVersion::parse(key_def.deprecated_in);
            if (version_ >= dep_ver) continue;
        }

        // Check key presence
        if (dict.find(key_def.key) == dict.end()) {
            // Special case: wildcard key "*" matches any key
            if (key_def.key == "*") continue;
            result_.add(Severity::Error, path, key_def.key,
                        "Required key missing", obj_def.name);
        }
    }

    // Validate each key present in the dictionary
    for (const auto& kv : dict) {
        const std::string& key = kv.first;
        const Value& val = kv.second;

        const KeyDefinition* key_def = obj_def.get_key(key);
        if (!key_def) {
            // Check for wildcard
            key_def = obj_def.get_key("*");
        }
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

        // Check possible values for Name type
        if (!key_def->possible_values.empty() && resolved_val.type == Value::NAME) {
            bool found = false;
            for (const auto& pv : key_def->possible_values) {
                if (pv == resolved_val.name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                result_.add(Severity::Warning, path, key,
                            "Value '" + resolved_val.name +
                            "' not in allowed values list",
                            obj_def.name);
            }
        }

        // Follow links recursively
        validate_value(pdf, resolved_val, *key_def, path + "." + key, depth + 1);
    }
}

void Validator::validate_value(const Pdf& pdf, const Value& val,
                               const KeyDefinition& key_def,
                               const std::string& path, int depth) {
    if (depth > kMaxDepth) return;
    if (key_def.links.empty()) return;

    // Only follow links for dictionary or stream values
    if (val.type != Value::DICTIONARY && val.type != Value::STREAM) return;

    const Dictionary& dict =
        (val.type == Value::STREAM) ? val.stream.dict : val.dict;

    // Try each link
    for (const auto& link : key_def.links) {
        if (link.empty()) continue;
        // Strip any fn: prefix or brackets
        std::string def_name = link;
        if (def_name.front() == '[') def_name = def_name.substr(1);
        if (!def_name.empty() && def_name.back() == ']')
            def_name.pop_back();

        const ObjectDefinition* linked_def = model_.get_object_def(def_name);
        if (!linked_def) continue;

        // Check for circular reference via object tracking
        // We use the path to detect recursion for inline dicts
        validate_dict(pdf, dict, *linked_def, path, depth);
        break;  // Only follow first matching link
    }
}

void Validator::validate_trailer(const Pdf& pdf) {
    const ObjectDefinition* trailer_def = model_.get_object_def("FileTrailer");
    if (!trailer_def) {
        // Try alternative names
        trailer_def = model_.get_object_def("Trailer");
    }
    if (!trailer_def) {
        result_.add(Severity::Info, "Trailer", "",
                    "FileTrailer definition not found in Arlington model", "");
        return;
    }

    validate_dict(pdf, pdf.trailer, *trailer_def, "Trailer", 0);
}

void Validator::validate_catalog(const Pdf& pdf) {
    const ObjectDefinition* catalog_def = model_.get_object_def("Catalog");
    if (!catalog_def) {
        result_.add(Severity::Warning, "Catalog", "",
                    "Catalog definition not found in Arlington model", "");
        return;
    }

    // Resolve root object to get the Catalog dictionary
    auto root_obj = resolve_reference(pdf, pdf.root, 0);
    if (!root_obj.success) {
        result_.add(Severity::Error, "Catalog", "",
                    "Could not resolve root object", "");
        return;
    }

    if (root_obj.value.type != Value::DICTIONARY) {
        result_.add(Severity::Error, "Catalog", "",
                    "Root object is not a dictionary", "");
        return;
    }

    validate_dict(pdf, root_obj.value.dict, *catalog_def, "Catalog", 0);
}

void Validator::validate_pages(const Pdf& pdf) {
    const ObjectDefinition* page_def = model_.get_object_def("PageObject");
    if (!page_def) {
        page_def = model_.get_object_def("Page");
    }
    if (!page_def) {
        result_.add(Severity::Info, "Pages", "",
                    "PageObject definition not found in Arlington model", "");
        return;
    }

    // Validate each page
    for (size_t i = 0; i < pdf.catalog.pages.size(); ++i) {
        const Page& page = pdf.catalog.pages[i];
        std::string path = "Page[" + std::to_string(i) + "]";

        // Resolve the page object to get its full dictionary
        auto page_obj = resolve_reference(pdf, page.object_number, 0);
        if (page_obj.success && page_obj.value.type == Value::DICTIONARY) {
            validate_dict(pdf, page_obj.value.dict, *page_def, path, 0);
        }
    }
}

ValidationResult Validator::validate_document(const Pdf& pdf) {
    result_ = ValidationResult();
    visited_.clear();
    version_ = PdfVersion(pdf.version_major, pdf.version_minor);

    result_.add(Severity::Info, "", "",
                "Validating PDF " + std::to_string(version_.major) + "." +
                std::to_string(version_.minor), "");

    validate_trailer(pdf);
    validate_catalog(pdf);
    validate_pages(pdf);

    return result_;
}

}  // namespace arlington
}  // namespace nanopdf
