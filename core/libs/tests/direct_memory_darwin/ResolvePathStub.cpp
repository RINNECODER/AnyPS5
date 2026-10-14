#include <filesystem>

// The test has no guest filesystem: guest paths resolve to themselves, which do not exist on the host,
// so the flexible memory budget falls back to its default.
extern "C" std::filesystem::path ResolvePath_nid_no_patch(const char* path) {
    return path;
}
