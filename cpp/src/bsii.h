#pragma once

// Bounded, pure in-memory converter for decrypted BSII (v3) byte streams.
//
// Wire format reference (documentation only, MPL-2.0; the implementation below
// is original C++ written from the protocol notes, not a port of the reference
// Pascal decoder): https://github.com/TheLazyTomcat/SII_Decrypt/tree/master/Documents
//
// On success decodeBsiiText returns true and *out holds complete "SiiNunit"
// text. On any failure it returns false, sets *error to a human readable
// reason, and clears *out. The decoder never touches the filesystem, never
// reads more than the supplied buffer, and is cancellable while parsing.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ets2 {

struct BsiiDecodeStats {
    uint32_t version = 0;     // BSII header version (3 on success today)
    size_t   schemaCount = 0; // declared schema blocks
    size_t   unitCount = 0;   // emitted data units
};

bool decodeBsiiText(const std::vector<uint8_t>& bytes,
                    std::string* out,
                    std::string* error,
                    std::function<bool()> canceled = {},
                    size_t maxOutputBytes = 128 * 1024 * 1024,
                    BsiiDecodeStats* stats = nullptr);

}  // namespace ets2
