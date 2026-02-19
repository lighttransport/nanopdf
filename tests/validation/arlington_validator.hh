#ifndef NANOPDF_ARLINGTON_VALIDATOR_HH
#define NANOPDF_ARLINGTON_VALIDATOR_HH

#include "arlington_parser.hh"
#include "nanopdf.hh"
#include <set>
#include <string>
#include <vector>

namespace nanopdf {
namespace arlington {

// ============================================================================
// Validation Result Types
// ============================================================================

enum class Severity {
    Info,
    Warning,
    Error
};

const char* severity_to_string(Severity sev);

struct ValidationFinding {
    Severity severity;
    std::string object_path;   // e.g., "Trailer.Root.Pages.Kids[0]"
    std::string key;           // The key that caused the finding
    std::string message;       // Human-readable description
    std::string arlington_ref; // Arlington TSV definition name
};

struct ValidationResult {
    bool success;
    int info_count;
    int warning_count;
    int error_count;
    std::vector<ValidationFinding> findings;

    ValidationResult()
        : success(true), info_count(0), warning_count(0), error_count(0) {}

    void add(Severity sev, const std::string& path, const std::string& key,
             const std::string& message, const std::string& ref);

    void print_summary() const;
    void print_findings(Severity min_severity = Severity::Warning) const;
};

// ============================================================================
// Validator - Recursive object graph walker
// ============================================================================

class Validator {
public:
    explicit Validator(const ArlingtonModel& model);

    /// Validate a parsed PDF document against the Arlington model.
    /// Recursively walks the entire reachable object graph starting from
    /// the file trailer.
    ValidationResult validate_document(const Pdf& pdf);

private:
    static const int kMaxDepth = 64;

    /// Map nanopdf Value::Type to ArlingtonType
    static ArlingtonType value_type_to_arlington(Value::Type vtype);

    /// Check if a value's type matches any of the allowed Arlington types
    static bool type_matches(const Value& val,
                             const std::vector<ArlingtonType>& allowed);

    // ---- Core recursive walker ----

    /// Single entry point: resolves refs, checks visited set, dispatches
    void validate_object(const Pdf& pdf, const Value& val,
                         const std::string& def_name,
                         const std::string& path, int depth);

    /// Validate dictionary keys against an object definition
    void validate_dict_keys(const Pdf& pdf, const Dictionary& dict,
                            const ObjectDefinition& obj_def,
                            const std::string& path, int depth);

    /// Validate array elements against an object definition
    void validate_array_elements(const Pdf& pdf,
                                  const std::vector<Value>& arr,
                                  const ObjectDefinition& obj_def,
                                  const std::string& path, int depth);

    // ---- Link following ----

    /// Follow links from a key definition to validate sub-objects
    void follow_links(const Pdf& pdf, const Value& val,
                      const KeyDefinition& key_def,
                      const std::string& path, int depth);

    /// Discriminate among multiple definition alternatives by Type/Subtype/S
    /// keys and key overlap scoring
    std::string choose_definition(const Pdf& pdf, const Dictionary& dict,
                                   const std::vector<std::string>& alternatives);

    /// Unwrap fn:SinceVersion/fn:Extension to get bare definition name
    static std::string extract_def_name(const std::string& link_entry);

    /// Map value type to positional index in types list
    static int find_type_index(const Value& val,
                                const std::vector<ArlingtonType>& types);

    // ---- Validation helpers ----

    /// Evaluate required expression against context dictionary
    static bool evaluate_required(const std::string& expr,
                                   const Dictionary& context_dict);

    /// Check indirect/direct reference requirements
    void check_indirect_reference(const Value& original_val,
                                   const std::string& expr,
                                   const std::string& path,
                                   const std::string& key,
                                   const std::string& def_name);

    /// Check value against possible values list
    void check_possible_values(const Value& val,
                                const std::vector<std::string>& possible_values,
                                const std::string& path,
                                const std::string& key,
                                const std::string& def_name);

    /// Resolve a Value reference if needed
    Value resolve(const Pdf& pdf, const Value& val) const;

    const ArlingtonModel& model_;
    ValidationResult result_;
    PdfVersion version_;
    std::set<uint32_t> visited_;  // Visited object numbers (cycle detection)
};

}  // namespace arlington
}  // namespace nanopdf

#endif  // NANOPDF_ARLINGTON_VALIDATOR_HH
