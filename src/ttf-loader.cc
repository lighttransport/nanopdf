#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>  // for memcpy
#include <map>

#include "stream-reader.hh"

// Custom implementation of ntohl
uint32_t custom_ntohl(uint32_t netlong) {
    return ((netlong & 0xFF000000) >> 24) |
           ((netlong & 0x00FF0000) >> 8)  |
           ((netlong & 0x0000FF00) << 8)  |
           ((netlong & 0x000000FF) << 24);
}

// Custom implementation of ntohs
uint16_t custom_ntohs(uint16_t netshort) {
    return ((netshort & 0xFF00) >> 8) |
           ((netshort & 0x00FF) << 8);
}

struct TTFHeader {
    uint32_t scalerType;
    uint16_t numTables;
    uint16_t searchRange;
    uint16_t entrySelector;
    uint16_t rangeShift;
};

struct TableRecord {
    char tag[4];
    uint32_t checksum;
    uint32_t offset;
    uint32_t length;
};

// CMap subtable entry
struct CMapSubtable {
    uint16_t platformID;
    uint16_t encodingID;
    uint16_t format;
    uint32_t offset;
};

// CMap table
struct CMap {
    uint16_t version;
    uint16_t numSubtables;
    std::vector<CMapSubtable> subtables;
};

class TrueTypeFontParser {
public:
    // Load from file, then dispatch to memory load
    bool load(const std::string& filepath, std::string* err) {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            if (err) *err = "Failed to open font file.";
            return false;
        }
        auto fileSize = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);
        dataBuffer.resize(fileSize);
        if (!file.read(reinterpret_cast<char*>(dataBuffer.data()), fileSize)) {
            if (err) *err = "Failed to read font file into memory buffer.";
            return false;
        }
        data = dataBuffer.data();
        dataSize = dataBuffer.size();
        return load(data, dataSize, err);
    }

    // Load from memory buffer
    bool load(const uint8_t* buf, size_t size, std::string* err) {
        data = buf;
        dataSize = size;
        if (size < sizeof(TTFHeader)) {
            if (err) *err = "Buffer too small for TTF header.";
            return false;
        }
        size_t offset = 0;
        memcpy(&header, data + offset, sizeof(header));
        offset += sizeof(header);

        header.scalerType   = custom_ntohl(header.scalerType);
        header.numTables    = custom_ntohs(header.numTables);
        header.searchRange  = custom_ntohs(header.searchRange);
        header.entrySelector= custom_ntohs(header.entrySelector);
        header.rangeShift   = custom_ntohs(header.rangeShift);

        tableRecords.clear();
        for (int i = 0; i < header.numTables; ++i) {
            if (offset + sizeof(TableRecord) > size) {
                if (err) *err = "Buffer too small for table record.";
                return false;
            }
            TableRecord rec;
            memcpy(&rec, data + offset, sizeof(rec));
            // convert multi-byte fields
            rec.checksum = custom_ntohl(rec.checksum);
            rec.offset   = custom_ntohl(rec.offset);
            rec.length   = custom_ntohl(rec.length);
            offset += sizeof(rec);
            tableRecords.push_back(rec);
        }
        return true;
    }

    // Load cmap table entries
    bool loadCMap(std::string* err) {
        // find cmap record
        uint32_t tableOffset = 0;
        for (auto& rec : tableRecords) {
            if (std::string(rec.tag,4) == "cmap") {
                tableOffset = rec.offset;
                break;
            }
        }
        if (tableOffset == 0 || tableOffset + 4 > dataSize) {
            if (err) *err = "Invalid or missing cmap table.";
            return false;
        }
        auto readU16 = [&](size_t off) {
            return (uint16_t(data[off]) << 8) | uint16_t(data[off+1]);
        };
        auto readU32 = [&](size_t off) {
            return (uint32_t(data[off])<<24) | (uint32_t(data[off+1])<<16) |
                   (uint32_t(data[off+2])<<8) | uint32_t(data[off+3]);
        };
        size_t off = tableOffset;
        cmap.version = readU16(off);
        cmap.numSubtables = readU16(off+2);
        off += 4;
        cmap.subtables.clear();
        for (int i = 0; i < cmap.numSubtables; ++i) {
            if (off + 8 > dataSize) {
                if (err) *err = "Malformed cmap encoding record.";
                return false;
            }
            CMapSubtable sub;
            sub.platformID = readU16(off);
            sub.encodingID = readU16(off+2);
            sub.offset     = readU32(off+4);
            size_t fmtOff = tableOffset + sub.offset;
            if (fmtOff + 2 > dataSize) {
                if (err) *err = "Malformed cmap subtable.";
                return false;
            }
            sub.format = readU16(fmtOff);
            cmap.subtables.push_back(sub);
            off += 8;
        }
        return true;
    }

    const CMap& getCMap() const { return cmap; }

    void printInfo() const {
        std::cout << "Scaler Type: " << header.scalerType << "\n";
        std::cout << "Number of Tables: " << header.numTables << "\n";
        for (const auto& record : tableRecords) {
            std::cout << "Table: " << std::string(record.tag, 4)
                      << ", Offset: " << record.offset
                      << ", Length: " << record.length << "\n";
        }
    }

private:
    TTFHeader header;
    std::vector<uint8_t> dataBuffer;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
    std::vector<TableRecord> tableRecords;
    CMap cmap;
};

int main() {
    TrueTypeFontParser parser;
    std::string err;
    if (!parser.load("example.ttf", &err)) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    if (!parser.loadCMap(&err)) {
        std::cerr << "cmap load error: " << err << "\n";
        return 1;
    }
    const auto& cmap = parser.getCMap();
    std::cout << "cmap version=" << cmap.version
              << " subtables=" << cmap.numSubtables << "\n";
    for (auto& s : cmap.subtables) {
        std::cout << "platform=" << s.platformID
                  << " encoding=" << s.encodingID
                  << " format=" << s.format
                  << " offset=" << s.offset << "\n";
    }
    return 0;
}