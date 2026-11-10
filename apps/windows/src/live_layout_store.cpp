#include "hop_windows/live_layout_store.h"
#include <algorithm>
#include <unordered_set>
#include <utility>

namespace hop::windows {
namespace {
constexpr std::size_t max_record = 256 * 1024;
constexpr std::size_t max_screens = 1024;

bool valid_id(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
}

bool valid_geometry(const std::vector<hop::Screen>& screens) {
    if (screens.empty() || screens.size() > max_screens) return false;
    std::unordered_set<std::string> ids;
    for (const auto& screen : screens) {
        if (!valid_id(screen.id) || !valid_id(screen.device_id) || !ids.insert(screen.id).second ||
            screen.width <= 0 || screen.width > 65535 || screen.height <= 0 || screen.height > 65535 ||
            screen.x < -10000000 || screen.x > 10000000 ||
            screen.y < -10000000 || screen.y > 10000000) return false;
    }
    return true;
}

const hop::Screen* matching_screen(const std::vector<hop::Screen>& screens, const hop::Screen& expected) {
    const auto match = std::find_if(screens.begin(), screens.end(), [&](const auto& screen) {
        return screen.id == expected.id && screen.device_id == expected.device_id &&
            screen.width == expected.width && screen.height == expected.height;
    });
    return match == screens.end() ? nullptr : &*match;
}

bool matches_layout(const std::vector<hop::Screen>& geometry, const LayoutEditor& editor) {
    const auto& screens = editor.layout().screens();
    return screens.size() == geometry.size() && std::all_of(geometry.begin(), geometry.end(), [&](const auto& native) {
        return matching_screen(screens, native) != nullptr;
    });
}

bool same_geometry(const std::vector<hop::Screen>& saved, const std::vector<hop::Screen>& expected) {
    return saved.size() == expected.size() && std::all_of(expected.begin(), expected.end(), [&](const auto& native) {
        const auto* match = matching_screen(saved, native);
        return match && match->x == native.x && match->y == native.y;
    });
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(value >> 8);
    out.push_back(value & 255);
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void put_id(std::vector<std::uint8_t>& out, const std::string& value) {
    out.push_back(static_cast<std::uint8_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t offset = 0;

    bool u16(std::uint16_t& value) {
        if (bytes.size() - offset < 2) return false;
        value = (std::uint16_t(bytes[offset]) << 8) | bytes[offset + 1];
        offset += 2;
        return true;
    }

    bool u32(std::uint32_t& value) {
        if (bytes.size() - offset < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8) | bytes[offset++];
        return true;
    }

    bool id(std::string& value) {
        if (offset == bytes.size()) return false;
        const auto size = bytes[offset++];
        if (!size || size > 128 || bytes.size() - offset < size) return false;
        value.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
        offset += size;
        return valid_id(value);
    }
};

std::int32_t signed32(std::uint32_t value) {
    return value <= 0x7fffffff ? static_cast<std::int32_t>(value) :
        static_cast<std::int32_t>(std::int64_t(value) - 0x100000000LL);
}
} // namespace

bool encode_live_layout(const LayoutEditor& editor, const std::vector<hop::Screen>& geometry,
                        std::vector<std::uint8_t>& bytes) {
    try {
        if (!valid_geometry(geometry) || !matches_layout(geometry, editor)) return false;
        std::vector<std::uint8_t> layout;
        if (!encode_layout(editor, layout)) return false;
        std::vector<std::uint8_t> out = {'H', 'L', 'S', '1'};
        put16(out, static_cast<std::uint16_t>(geometry.size()));
        for (const auto& screen : geometry) {
            put_id(out, screen.id);
            put_id(out, screen.device_id);
            put32(out, static_cast<std::uint32_t>(screen.x));
            put32(out, static_cast<std::uint32_t>(screen.y));
            put32(out, static_cast<std::uint32_t>(screen.width));
            put32(out, static_cast<std::uint32_t>(screen.height));
            if (out.size() > max_record) return false;
        }
        if (sizeof(std::uint32_t) + layout.size() > max_record - out.size()) return false;
        put32(out, static_cast<std::uint32_t>(layout.size()));
        out.insert(out.end(), layout.begin(), layout.end());
        bytes = std::move(out);
        return true;
    } catch (...) {
        return false;
    }
}

LiveLayoutRestore decode_live_layout(const std::vector<std::uint8_t>& bytes,
                                     const std::vector<hop::Screen>& expected, LayoutEditor& editor) {
    try {
        if (!valid_geometry(expected) || bytes.size() < 10 || bytes.size() > max_record ||
            bytes[0] != 'H' || bytes[1] != 'L' || bytes[2] != 'S' || bytes[3] != '1') return LiveLayoutRestore::error;
        Reader reader{bytes, 4};
        std::uint16_t count = 0;
        if (!reader.u16(count) || !count || count > max_screens) return LiveLayoutRestore::error;
        std::vector<hop::Screen> geometry;
        geometry.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            hop::Screen screen{};
            std::uint32_t x = 0, y = 0, width = 0, height = 0;
            if (!reader.id(screen.id) || !reader.id(screen.device_id) ||
                !reader.u32(x) || !reader.u32(y) || !reader.u32(width) || !reader.u32(height) ||
                width > 65535 || height > 65535) return LiveLayoutRestore::error;
            screen.x = signed32(x);
            screen.y = signed32(y);
            screen.width = static_cast<std::int32_t>(width);
            screen.height = static_cast<std::int32_t>(height);
            geometry.push_back(std::move(screen));
        }
        std::uint32_t length = 0;
        if (!reader.u32(length) || length != bytes.size() - reader.offset || !valid_geometry(geometry))
            return LiveLayoutRestore::error;
        const std::vector<std::uint8_t> layout(bytes.begin() + reader.offset, bytes.end());
        LayoutEditor candidate;
        if (!decode_layout(layout, candidate) || !matches_layout(geometry, candidate)) return LiveLayoutRestore::error;
        if (!same_geometry(geometry, expected)) return LiveLayoutRestore::geometry_mismatch;
        editor = std::move(candidate);
        return LiveLayoutRestore::restored;
    } catch (...) {
        return LiveLayoutRestore::error;
    }
}

bool save_live_layout(LayoutStorage& storage, const LayoutEditor& editor, const std::vector<hop::Screen>& geometry) {
    try {
        std::vector<std::uint8_t> bytes, previous;
        bool found = false;
        LayoutEditor ignored;
        if (!encode_live_layout(editor, geometry, bytes) || !storage.load(previous, found) ||
            (found && decode_live_layout(previous, geometry, ignored) == LiveLayoutRestore::error)) return false;
        return storage.save(bytes);
    } catch (...) {
        return false;
    }
}

LiveLayoutRestore load_live_layout(LayoutStorage& storage, const std::vector<hop::Screen>& geometry, LayoutEditor& editor) {
    try {
        std::vector<std::uint8_t> bytes;
        bool found = false;
        if (!valid_geometry(geometry) || !matches_layout(geometry, editor) || !storage.load(bytes, found))
            return LiveLayoutRestore::error;
        if (!found) return LiveLayoutRestore::missing;
        LayoutEditor saved;
        const auto result = decode_live_layout(bytes, geometry, saved);
        if (result != LiveLayoutRestore::restored) return result;
        auto live = editor.devices();
        for (auto& device : live) {
            const auto previous = std::find_if(saved.devices().begin(), saved.devices().end(), [&](const auto& candidate) {
                return candidate.id == device.id;
            });
            if (previous == saved.devices().end()) return LiveLayoutRestore::error;
            device.expanded = previous->expanded;
            for (auto& screen : device.screens) {
                const auto* placement = matching_screen(previous->screens, screen);
                if (!placement) return LiveLayoutRestore::error;
                screen.x = placement->x;
                screen.y = placement->y;
            }
        }
        return editor.replace(std::move(live)) ? LiveLayoutRestore::restored : LiveLayoutRestore::error;
    } catch (...) {
        return LiveLayoutRestore::error;
    }
}
} // namespace hop::windows
