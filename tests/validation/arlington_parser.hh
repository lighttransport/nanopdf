#ifndef NANOPDF_ARLINGTON_PARSER_HH
#define NANOPDF_ARLINGTON_PARSER_HH

#include <map>
#include <string>
#include <vector>

namespace nanopdf {
namespace arlington {

// ============================================================================
// Arlington Type System
// ============================================================================

enum class ArlingtonType {
    Array,
    Bitmask,
    Boolean,
    Dictionary,
    Integer,
    Name,
    Null,
    Number,
    Stream,
    String,
    NameTree,
    NumberTree,
    Rectangle,
    Matrix,
    Date,
    Unknown
};

/// Convert type string from TSV to enum
ArlingtonType parse_type(const std::string& type_str);

/// Convert enum to string for display
const char* type_to_string(ArlingtonType type);

// ============================================================================
// Link Group (positional per type alternative)
// ============================================================================

struct LinkGroup {
    std::vector<std::string> alternatives;
};

// ============================================================================
// Key Definition (one row from a TSV file)
// ============================================================================

struct KeyDefinition {
    std::string key;                       // Column 0: Key name (e.g., "Type", "Pages")
    std::vector<ArlingtonType> types;      // Column 1: Allowed types (semicolon-separated)
    std::string since_version;             // Column 2: Since PDF version (e.g., "1.0")
    std::string deprecated_in;             // Column 3: Deprecated in version
    std::string required_expr;             // Column 4: Required expression (raw)
    std::string indirect_reference;        // Column 5: Must be indirect reference?
    bool inheritable{false};               // Column 6: Inheritable
    std::string default_value;             // Column 7: Default value
    std::vector<std::string> possible_values; // Column 8: Possible values
    std::string special_case;              // Column 9: Special case
    std::vector<LinkGroup> link_groups;    // Column 10: Links (positional per type)
    std::string notes;                     // Column 11+: Additional notes

    /// Check if unconditionally required (TRUE)
    bool is_unconditionally_required() const {
        return required_expr == "TRUE" || required_expr == "true" ||
               required_expr == "1";
    }
};

// ============================================================================
// Object Definition (one complete TSV file)
// ============================================================================

struct ObjectDefinition {
    std::string name;                      // Filename without .tsv extension
    std::vector<KeyDefinition> keys;       // All key definitions
    std::map<std::string, size_t> key_index; // Key name -> index in keys vector

    /// Look up a key definition by name
    const KeyDefinition* get_key(const std::string& key_name) const;
};

// ============================================================================
// Arlington Model (all TSV files loaded)
// ============================================================================

class ArlingtonModel {
public:
    ArlingtonModel() = default;

    /// Load all TSV files from a directory
    /// @param dir_path Path to directory containing .tsv files
    /// @return true on success
    bool load(const std::string& dir_path);

    /// Look up an object definition by name
    /// @param name Object type name (e.g., "Catalog", "FileTrailer")
    /// @return Pointer to definition or nullptr
    const ObjectDefinition* get_object_def(const std::string& name) const;

    /// Get number of loaded definitions
    size_t definition_count() const { return definitions_.size(); }

    /// Get all definition names
    std::vector<std::string> definition_names() const;

    /// Check if model is loaded
    bool is_loaded() const { return !definitions_.empty(); }

private:
    /// Parse a single TSV file
    bool parse_tsv_file(const std::string& filepath);

    /// Parse a single line from a TSV file into a KeyDefinition
    static KeyDefinition parse_tsv_line(const std::string& line);

    /// Parse semicolon-separated type list
    static std::vector<ArlingtonType> parse_types(const std::string& type_str);

    /// Parse bracket-delimited value list with positional groups
    static std::vector<std::string> parse_value_list(const std::string& str);

    /// Parse link groups (positional per type alternative)
    static std::vector<LinkGroup> parse_link_groups(const std::string& str);

    /// Extract object name from file path
    static std::string extract_name(const std::string& filepath);

    std::map<std::string, ObjectDefinition> definitions_;
};

// ============================================================================
// Version Comparison
// ============================================================================

struct PdfVersion {
    int major;
    int minor;

    PdfVersion() : major(1), minor(0) {}
    PdfVersion(int maj, int min) : major(maj), minor(min) {}

    /// Parse version string like "1.7" or "2.0"
    static PdfVersion parse(const std::string& ver_str);

    bool operator<(const PdfVersion& other) const;
    bool operator<=(const PdfVersion& other) const;
    bool operator>(const PdfVersion& other) const;
    bool operator>=(const PdfVersion& other) const;
    bool operator==(const PdfVersion& other) const;
};

}  // namespace arlington
}  // namespace nanopdf

#endif  // NANOPDF_ARLINGTON_PARSER_HH
