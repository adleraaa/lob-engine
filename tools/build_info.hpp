// Build provenance written into every results file (difftest.json,
// bench.json), so a committed number can be tied to the exact source
// revision, compiler and flags that produced it. The macros come from
// CMakeLists.txt; a build outside CMake (the mutation script compiles the
// difftest directly) gets "unknown" and the script records its own values.
#pragma once

#include <string>

#ifndef LOB_GIT_SHA
#define LOB_GIT_SHA "unknown"
#endif
#ifndef LOB_BUILD_TYPE
#define LOB_BUILD_TYPE "unknown"
#endif
#ifndef LOB_BUILD_FLAGS
#define LOB_BUILD_FLAGS "unknown"
#endif
#ifndef LOB_LINK_FLAGS
#define LOB_LINK_FLAGS ""
#endif

namespace lob::build {

inline std::string compiler() {
#if defined(__clang__)
    return __VERSION__;  // already starts with "Clang"
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "unknown";
#endif
}

inline const char* os() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#else
    return "other";
#endif
}

inline bool assertions_enabled() {
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
}

// Escapes the two characters that would break a JSON string literal here.
inline std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// The shared "build" object, as JSON text.
inline std::string json_object() {
    return std::string("{\"git_sha\": \"") + json_escape(LOB_GIT_SHA) + "\", \"compiler\": \"" +
           json_escape(compiler()) + "\", \"build_type\": \"" + json_escape(LOB_BUILD_TYPE) + "\", \"flags\": \"" +
           json_escape(LOB_BUILD_FLAGS) + "\", \"link_flags\": \"" + json_escape(LOB_LINK_FLAGS) +
           "\", \"assertions\": " + (assertions_enabled() ? "true" : "false") + ", \"os\": \"" + os() + "\"}";
}

}  // namespace lob::build
