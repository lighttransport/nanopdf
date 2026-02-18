#include "arlington_parser.hh"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <dirent.h>

namespace nanopdf {
namespace arlington {

// ============================================================================
// ArlingtonType parsing
// ============================================================================

ArlingtonType parse_type(const std::string& type_str) {
    // Trim whitespace
    std::string s = type_str;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();

    if (s == "array")       return ArlingtonType::Array;
    if (s == "boolean")     return ArlingtonType::Boolean;
    if (s == "dictionary")  return ArlingtonType::Dictionary;
    if (s == "integer")     return ArlingtonType::Integer;
    if (s == "name")        return ArlingtonType::Name;
    if (s == "null")        return ArlingtonType::Null;
    if (s == "number")      return ArlingtonType::Number;
    if (s == "stream")      return ArlingtonType::Stream;
    if (s == "string")      return ArlingtonType::String;
    if (s == "name-tree")   return ArlingtonType::NameTree;
    if (s == "number-tree") return ArlingtonType::NumberTree;
    if (s == "rectangle")   return ArlingtonType::Rectangle;
    if (s == "matrix")      return ArlingtonType::Matrix;
    if (s == "date")        return ArlingtonType::Date;
    if (s == "string-byte") return ArlingtonType::String;
    if (s == "string-ascii") return ArlingtonType::String;
    if (s == "string-text") return ArlingtonType::String;
    return ArlingtonType::Unknown;
}

const char* type_to_string(ArlingtonType type) {
    switch (type) {
        case ArlingtonType::Array:      return "array";
        case ArlingtonType::Boolean:    return "boolean";
        case ArlingtonType::Dictionary: return "dictionary";
        case ArlingtonType::Integer:    return "integer";
        case ArlingtonType::Name:       return "name";
        case ArlingtonType::Null:       return "null";
        case ArlingtonType::Number:     return "number";
        case ArlingtonType::Stream:     return "stream";
        case ArlingtonType::String:     return "string";
        case ArlingtonType::NameTree:   return "name-tree";
        case ArlingtonType::NumberTree: return "number-tree";
        case ArlingtonType::Rectangle:  return "rectangle";
        case ArlingtonType::Matrix:     return "matrix";
        case ArlingtonType::Date:       return "date";
        case ArlingtonType::Unknown:    return "unknown";
    }
    return "unknown";
}

// ============================================================================
// ObjectDefinition
// ============================================================================

const KeyDefinition* ObjectDefinition::get_key(const std::string& key_name) const {
    auto it = key_index.find(key_name);
    if (it == key_index.end()) return nullptr;
    return &keys[it->second];
}

// ============================================================================
// ArlingtonModel - TSV parsing
// ============================================================================

static std::vector<std::string> split_tsv(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\t') {
            fields.push_back(field);
            field.clear();
        } else {
            field += line[i];
        }
    }
    fields.push_back(field);
    return fields;
}

std::vector<ArlingtonType> ArlingtonModel::parse_types(const std::string& type_str) {
    std::vector<ArlingtonType> result;
    if (type_str.empty()) return result;

    std::string s = type_str;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t next = s.find(';', pos);
        if (next == std::string::npos) next = s.size();
        std::string token = s.substr(pos, next - pos);
        // Strip fn: prefix if present (Arlington uses fn:SinceVersion(...) etc.)
        if (token.find("fn:") == std::string::npos) {
            ArlingtonType t = parse_type(token);
            if (t != ArlingtonType::Unknown || !token.empty()) {
                result.push_back(t);
            }
        }
        pos = next + 1;
    }
    return result;
}

std::vector<std::string> ArlingtonModel::parse_value_list(const std::string& str) {
    std::vector<std::string> result;
    if (str.empty()) return result;

    std::string s = str;
    // Strip brackets if present
    if (s.front() == '[') s = s.substr(1);
    if (!s.empty() && s.back() == ']') s.pop_back();

    size_t pos = 0;
    while (pos < s.size()) {
        size_t next = s.find(',', pos);
        if (next == std::string::npos) next = s.size();
        std::string val = s.substr(pos, next - pos);
        // Trim
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        while (!val.empty() && val.back() == ' ') val.pop_back();
        if (!val.empty()) {
            result.push_back(val);
        }
        pos = next + 1;
    }
    return result;
}

std::vector<std::string> ArlingtonModel::parse_links(const std::string& str) {
    std::vector<std::string> result;
    if (str.empty()) return result;

    std::string s = str;
    // Strip brackets
    if (s.front() == '[') s = s.substr(1);
    if (!s.empty() && s.back() == ']') s.pop_back();

    size_t pos = 0;
    while (pos < s.size()) {
        size_t next = s.find(';', pos);
        if (next == std::string::npos) next = s.size();
        std::string val = s.substr(pos, next - pos);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        while (!val.empty() && val.back() == ' ') val.pop_back();
        if (!val.empty()) {
            result.push_back(val);
        }
        pos = next + 1;
    }
    return result;
}

KeyDefinition ArlingtonModel::parse_tsv_line(const std::string& line) {
    KeyDefinition def;
    auto fields = split_tsv(line);

    if (fields.size() > 0) def.key = fields[0];
    if (fields.size() > 1) def.types = parse_types(fields[1]);
    if (fields.size() > 2) def.since_version = fields[2];
    if (fields.size() > 3) def.deprecated_in = fields[3];
    if (fields.size() > 4) {
        std::string req = fields[4];
        // Handle fn: expressions - treat as conditional (not strictly required)
        if (req == "TRUE" || req == "true" || req == "1") {
            def.required = true;
        } else {
            def.required = false;
        }
    }
    if (fields.size() > 5) def.indirect_reference = fields[5];
    if (fields.size() > 6) def.default_value = fields[6];
    if (fields.size() > 7) def.possible_values = parse_value_list(fields[7]);
    if (fields.size() > 8) def.special_case = fields[8];
    if (fields.size() > 9) def.links = parse_links(fields[9]);
    if (fields.size() > 10) {
        // Join remaining fields as notes
        std::string notes;
        for (size_t i = 10; i < fields.size(); ++i) {
            if (!notes.empty()) notes += "\t";
            notes += fields[i];
        }
        def.notes = notes;
    }

    return def;
}

std::string ArlingtonModel::extract_name(const std::string& filepath) {
    // Get filename without path
    size_t slash = filepath.rfind('/');
    std::string filename = (slash != std::string::npos) ?
        filepath.substr(slash + 1) : filepath;
    // Remove .tsv extension
    size_t dot = filename.rfind(".tsv");
    if (dot != std::string::npos) {
        filename = filename.substr(0, dot);
    }
    return filename;
}

bool ArlingtonModel::parse_tsv_file(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return false;

    ObjectDefinition obj_def;
    obj_def.name = extract_name(filepath);

    std::string line;
    bool first_line = true;
    while (std::getline(file, line)) {
        // Remove trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Skip empty lines
        if (line.empty()) continue;
        // Skip header line
        if (first_line) {
            first_line = false;
            // Check if this looks like a header
            if (line.find("Key") == 0 || line.find("key") == 0 ||
                line.find("Type") == 0) {
                continue;
            }
        }

        KeyDefinition key_def = parse_tsv_line(line);
        if (!key_def.key.empty()) {
            obj_def.key_index[key_def.key] = obj_def.keys.size();
            obj_def.keys.push_back(std::move(key_def));
        }
    }

    if (!obj_def.keys.empty()) {
        definitions_[obj_def.name] = std::move(obj_def);
    }
    return true;
}

bool ArlingtonModel::load(const std::string& dir_path) {
    definitions_.clear();

    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
        std::cerr << "[arlington] Cannot open directory: " << dir_path << "\n";
        return false;
    }

    struct dirent* entry;
    int file_count = 0;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (name.size() < 5) continue;
        if (name.substr(name.size() - 4) != ".tsv") continue;

        std::string filepath = dir_path + "/" + name;
        if (parse_tsv_file(filepath)) {
            file_count++;
        }
    }
    closedir(dir);

    std::cout << "[arlington] Loaded " << file_count << " TSV files ("
              << definitions_.size() << " definitions)\n";
    return !definitions_.empty();
}

const ObjectDefinition* ArlingtonModel::get_object_def(const std::string& name) const {
    auto it = definitions_.find(name);
    if (it == definitions_.end()) return nullptr;
    return &it->second;
}

std::vector<std::string> ArlingtonModel::definition_names() const {
    std::vector<std::string> names;
    names.reserve(definitions_.size());
    for (const auto& pair : definitions_) {
        names.push_back(pair.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// ============================================================================
// PdfVersion
// ============================================================================

PdfVersion PdfVersion::parse(const std::string& ver_str) {
    PdfVersion v;
    if (ver_str.empty()) return v;

    size_t dot = ver_str.find('.');
    if (dot != std::string::npos) {
        try {
            v.major = std::stoi(ver_str.substr(0, dot));
            v.minor = std::stoi(ver_str.substr(dot + 1));
        } catch (...) {
            v.major = 1;
            v.minor = 0;
        }
    } else {
        try {
            v.major = std::stoi(ver_str);
            v.minor = 0;
        } catch (...) {
            v.major = 1;
            v.minor = 0;
        }
    }
    return v;
}

bool PdfVersion::operator<(const PdfVersion& other) const {
    if (major != other.major) return major < other.major;
    return minor < other.minor;
}

bool PdfVersion::operator<=(const PdfVersion& other) const {
    return !(other < *this);
}

bool PdfVersion::operator>(const PdfVersion& other) const {
    return other < *this;
}

bool PdfVersion::operator>=(const PdfVersion& other) const {
    return !(*this < other);
}

bool PdfVersion::operator==(const PdfVersion& other) const {
    return major == other.major && minor == other.minor;
}

}  // namespace arlington
}  // namespace nanopdf
