#include "hop_windows/live_layout_store.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace hop::windows;

namespace {
struct Memory : LayoutStorage {
    bool exists = false, fail_read = false, fail_write = false;
    unsigned saves = 0;
    std::vector<std::uint8_t> bytes;

    bool load(std::vector<std::uint8_t>& out, bool& found) override {
        if (fail_read) return false;
        found = exists;
        if (found) out = bytes;
        return true;
    }

    bool save(const std::vector<std::uint8_t>& value) override {
        if (fail_write) return false;
        bytes = value;
        exists = true;
        ++saves;
        return true;
    }
};
}

int main() {
    int checks = 0;
    auto check = [&](bool value, const char* label) {
        ++checks;
        if (!value) {
            std::cerr << "FAIL " << label << '\n';
            std::exit(1);
        }
    };
    const std::string peer(64, 'A');
    LayoutEditor editor;
    std::vector<LayoutDevice> devices = {
        {"local", L"Local", true, true, {{"l:1", "local", -100, 0, 100, 100}}, true},
        {peer, L"Remote", true, true, {{"r:7", peer, -150, 0, 50, 100}}, true}
    };
    const std::vector<hop::Screen> native = {
        {"l:1", "local", -100, 0, 100, 100},
        {"r:7", peer, 0, -60, 50, 100}
    };
    check(editor.replace(devices), "original placement");
    Memory storage;
    check(save_live_layout(storage, editor, native), "save current topology");

    LayoutEditor offline;
    check(decode_live_layout(storage.bytes, native, offline) == LiveLayoutRestore::restored, "decode matching geometry");
    check(offline.devices()[1].screens[0].x == -150 && !offline.devices()[1].trusted && !offline.devices()[1].online,
          "decode positions without trust or online state");

    LayoutEditor live;
    devices[1].name = L"Current peer name";
    devices[1].expanded = false;
    devices[1].screens[0].x = 0;
    check(live.replace(devices) && live.select(peer), "default live layout and selection");
    auto reversed = native;
    std::reverse(reversed.begin(), reversed.end());
    check(load_live_layout(storage, reversed, live) == LiveLayoutRestore::restored, "restore regardless of geometry order");
    check(live.devices()[1].screens[0].x == -150 && live.devices()[1].expanded, "apply saved positions and expansion");
    check(live.devices()[1].name == L"Current peer name" && live.devices()[1].trusted && live.devices()[1].online &&
          live.selected_device() == peer, "preserve authenticated live metadata and selection");

    auto changed = native;
    changed[1].x = 1;
    check(load_live_layout(storage, changed, live) == LiveLayoutRestore::geometry_mismatch &&
          live.devices()[1].screens[0].x == -150, "changed native x leaves editor unchanged");
    changed[1].y = 1;
    changed[1].x = native[1].x;
    check(decode_live_layout(storage.bytes, changed, offline) == LiveLayoutRestore::geometry_mismatch, "changed native y");
    changed = native;
    changed[1].device_id = std::string(64, 'B');
    check(decode_live_layout(storage.bytes, changed, offline) == LiveLayoutRestore::geometry_mismatch, "changed identity");
    changed = native;
    changed[1].width = 51;
    check(decode_live_layout(storage.bytes, changed, offline) == LiveLayoutRestore::geometry_mismatch, "changed width");
    changed = native;
    changed[1].height = 101;
    check(decode_live_layout(storage.bytes, changed, offline) == LiveLayoutRestore::geometry_mismatch, "changed height");
    changed = native;
    changed[1].id = "r:8";
    check(decode_live_layout(storage.bytes, changed, offline) == LiveLayoutRestore::geometry_mismatch, "changed screen ID");
    changed = native;
    changed[1].id = changed[0].id;
    check(decode_live_layout(storage.bytes, changed, offline) == LiveLayoutRestore::error, "duplicate live screen ID");

    const auto good = storage.bytes;
    for (std::size_t length = 0; length < good.size(); ++length) {
        const std::vector<std::uint8_t> truncated(good.begin(), good.begin() + length);
        check(decode_live_layout(truncated, native, offline) == LiveLayoutRestore::error, "reject every truncated prefix");
    }
    const auto rejects_corruption = [&](const char* label) {
        const auto corrupt = storage.bytes;
        check(load_live_layout(storage, native, live) == LiveLayoutRestore::error &&
              live.devices()[1].screens[0].x == -150, label);
        check(!save_live_layout(storage, editor, native) && storage.bytes == corrupt && storage.saves == 1,
              "preserve corrupt record when save is requested");
        storage.bytes = good;
    };
    storage.bytes.push_back(0);
    rejects_corruption("reject trailing byte");
    storage.bytes[0] = 'X';
    rejects_corruption("reject unknown format");
    storage.bytes[4] = 0xff;
    rejects_corruption("reject excess geometry count");
    storage.bytes[6] = 0;
    rejects_corruption("reject missing screen identifier");
    // The first native screen begins after six header bytes, a four-byte
    // screen identifier and a six-byte owner identifier. Change its width.
    storage.bytes[27] = 0;
    rejects_corruption("reject zero width in native geometry");
    const std::vector<std::uint8_t> embedded_magic = {'H','L','Y','1'};
    const auto inner = std::search(storage.bytes.begin(), storage.bytes.end(), embedded_magic.begin(), embedded_magic.end());
    check(inner != storage.bytes.end(), "find embedded HLY1");
    *inner = 'X';
    rejects_corruption("reject malformed embedded editor");
    storage.bytes.resize(256 * 1024 + 1);
    rejects_corruption("reject oversized record");

    storage.fail_write = true;
    check(!save_live_layout(storage, editor, native) && storage.bytes == good, "write failure preserves record");
    storage.fail_write = false;
    storage.fail_read = true;
    check(load_live_layout(storage, native, live) == LiveLayoutRestore::error &&
          !save_live_layout(storage, editor, native) && storage.bytes == good, "read failure forbids overwriting");
    storage.fail_read = false;
    Memory missing;
    check(load_live_layout(missing, native, live) == LiveLayoutRestore::missing, "missing record leaves defaults");
    changed = native;
    changed.pop_back();
    check(!save_live_layout(storage, editor, changed) && storage.bytes == good, "layout/native size mismatch");
    changed = native;
    changed[1].x = 1;
    check(save_live_layout(storage, editor, changed) && storage.saves == 2 &&
          load_live_layout(storage, native, live) == LiveLayoutRestore::geometry_mismatch &&
          load_live_layout(storage, changed, live) == LiveLayoutRestore::restored, "explicit edit can replace old topology");
    std::cout << "PASS " << checks << " verified live-layout persistence checks; fake storage only\n";
}
