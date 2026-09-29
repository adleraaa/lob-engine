// Command-line parsing helpers shared by lob_difftest and lob_bench.
//
// Both programs are gates (the difftest decides pass/fail, the bench writes
// committed numbers), so a typo must be an error, not a silent default:
// std::strtoull("abc") returns 0, and "--ops abc" used to run zero requests
// and report OK.
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>

namespace lob::cli {

// Parses the whole of `text` as a decimal unsigned integer. Returns false for
// an empty string, a sign, trailing characters ("12abc") or overflow.
inline bool parse_u64(const char* text, std::uint64_t& out) {
    if (text == nullptr || *text < '0' || *text > '9') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno == ERANGE || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

}  // namespace lob::cli
