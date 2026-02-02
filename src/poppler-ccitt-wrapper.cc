// SPDX-License-Identifier: GPL-2.0-or-later
// Wrapper to use Poppler's CCITT decoder in nanopdf
// This file links against libpoppler which is GPL-licensed

#ifdef NANOPDF_USE_POPPLER_CCITT

#include "poppler-ccitt-wrapper.hh"
#include <vector>
#include <cstring>

// Poppler headers (from ref/poppler/poppler/)
#include "Stream.h"
#include "Object.h"

namespace nanopdf {
namespace poppler_ccitt {

// Memory stream to feed data to poppler's CCITTFaxStream
class MemStream : public BaseStream {
public:
    MemStream(const uint8_t* data, size_t size)
        : BaseStream(nullptr, 0)
    {
        buf = const_cast<char*>(reinterpret_cast<const char*>(data));
        length = size;
        pos = 0;
    }

    ~MemStream() override = default;

    Stream *copy() override {
        return new MemStream(reinterpret_cast<const uint8_t*>(buf), length);
    }

    BaseStream *getBaseStream() override { return this; }
    Stream *getUndecodedStream() override { return this; }

    StreamKind getKind() const override { return strFile; }

    void reset() override { pos = 0; }

    void close() override { }

    int getChar() override {
        if (pos >= length) return EOF;
        return (unsigned char)buf[pos++];
    }

    int lookChar() override {
        if (pos >= length) return EOF;
        return (unsigned char)buf[pos];
    }

    Goffset getPos() override { return pos; }

    void setPos(Goffset posA, int dir = 0) override {
        if (dir < 0) {
            pos = length + posA;
        } else if (dir > 0) {
            pos = posA;
        } else {
            pos = posA;
        }
        if (pos < 0) pos = 0;
        if (pos > length) pos = length;
    }

    void moveStart(Goffset delta) override { }

    bool isBinary(bool last = true) const override { return true; }

private:
    char* buf;
    size_t length;
    size_t pos;
};

// Wrapper function with C++ linkage
std::vector<uint8_t> decode_ccitt_poppler_wrapper(const uint8_t* data, size_t size,
                                                    int encoding, bool endOfLine,
                                                    bool byteAlign, int columns,
                                                    int rows, bool endOfBlock,
                                                    bool blackIs1) {
    std::vector<uint8_t> result;

    // Create memory stream with compressed data
    MemStream* memStream = new MemStream(data, size);

    // Create CCITT decoder
    // encoding: K parameter (<0=Group4, 0=Group3-1D, >0=Group3-2D)
    // damagedRowsBeforeError: 0 = report errors immediately
    CCITTFaxStream* ccittStream = new CCITTFaxStream(
        memStream,
        encoding,
        endOfLine,
        byteAlign,
        columns,
        rows,
        endOfBlock,
        blackIs1,
        0  // damagedRowsBeforeError
    );

    // Reset stream to start decoding
    ccittStream->reset();

    // Read all decoded bytes
    int c;
    while ((c = ccittStream->getChar()) != EOF) {
        result.push_back(static_cast<uint8_t>(c));
    }

    // Clean up
    delete ccittStream;  // This also deletes memStream

    return result;
}

#endif // NANOPDF_USE_POPPLER_CCITT
