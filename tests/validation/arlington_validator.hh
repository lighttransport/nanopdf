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
    std::string object_path;   // e.g., "Catalog.Pages.Kids[0]"
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
// Validator
// ============================================================================

class Validator {
public:
    explicit Validator(const ArlingtonModel& model);

    /// Validate a parsed PDF document against the Arlington model
    /// @param pdf The parsed PDF document
    /// @return Validation results
    ValidationResult validate_document(const Pdf& pdf);

private:
    static const int kMaxDepth = 16;

    /// Map nanopdf Value::Type to ArlingtonType
    static ArlingtonType value_type_to_arlington(Value::Type vtype);

    /// Check if a value's type matches any of the allowed Arlington types
    static bool type_matches(const Value& val,
                             const std::vector<ArlingtonType>& allowed);

    /// Validate a dictionary against an Arlington object definition
    void validate_dict(const Pdf& pdf, const Dictionary& dict,
                       const ObjectDefinition& obj_def,
                       const std::string& path, int depth);

    /// Validate a value and follow links recursively
    void validate_value(const Pdf& pdf, const Value& val,
                        const KeyDefinition& key_def,
                        const std::string& path, int depth);

    /// Validate the file trailer
    void validate_trailer(const Pdf& pdf);

    /// Validate the document catalog
    void validate_catalog(const Pdf& pdf);

    /// Validate the page tree
    void validate_pages(const Pdf& pdf);

    /// Resolve a Value reference if needed
    Value resolve(const Pdf& pdf, const Value& val) const;

    const ArlingtonModel& model_;
    ValidationResult result_;
    PdfVersion version_;
    std::set<uint32_t> visited_;  // Visited object numbers
};

}  // namespace arlington
}  // namespace nanopdf

#endif  // NANOPDF_ARLINGTON_VALIDATOR_HH
