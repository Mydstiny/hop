// Test-only host driver for the ACTUAL HarmonyOS Client, linked to recording fakes.
// No system input library, NAPI, device API, disk token store, or physical capture.
#include "client.h"
#include "generated/keymap.h"
#include <algorithm>
#include <multimodalinput/oh_input_manager.h>
#include <atomic>
#include <iostream>
#include <mutex>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::mutex output;
    std::atomic<int> persisted{0}, local{0}, remote{0}, leaving{0}, errors{0};
    auto log = [&](const char* value) { std::lock_guard<std::mutex> lock(output); std::cout << value << std::endl; };
    hop::Client* active = nullptr;
    hop::Client client([&](hop::Event event) {
        if (event.kind == "pairing") {
            log("CHALLENGE");
            active->Command("confirm"); // explicit TEST-only panel; production never auto-confirms
        } else if (event.kind == "persist") {
            ++persisted; // memory-only durability fixture, not real Preferences
            active->Command("saved:" + event.session);
            log("PERSISTED_IN_MEMORY");
        } else if (event.kind == "error") {
            ++errors; log("ERROR"); // no token or other secret is printed
        } else if (event.kind == "state" && event.state == "local") {
            if (++local == 1) log("READY");
            else if (fake::Empty() && fake::caps == 0) log("LOCAL_RELEASED");
            else { ++errors; log("ERROR"); }
        } else if (event.kind == "state" && event.state == "remote") {
            ++remote;
            bool initialHeld;
            { std::lock_guard<std::mutex> lock(fake::mutex);
              initialHeld = fake::buttons.count(MOUSE_BUTTON_LEFT) && fake::keys.count(hop::MapMacKey(56)); }
            if (initialHeld) log("REMOTE_HELD");
            else { ++errors; log("ERROR"); }
        } else if (event.kind == "state" && event.state == "leaving") {
            ++leaving;
            if (fake::Empty() && fake::caps == 0) log("LEAVING_RELEASED");
            else { ++errors; log("ERROR"); }
        }
    });
    active = &client;
    hop::Config config;
    config.host = "127.0.0.1";
    config.port = std::stoi(argv[1]);
    config.width = 1920; config.height = 1080;
    config.uuid = std::string(32, '2'); config.name = "Native interoperability test";
    client.Start(config);
    std::string command;
    while (std::getline(std::cin, command)) if (command == "quit") break;
    client.Stop();
    auto keyCount = [](uint16_t mac, int action) {
        return std::count_if(fake::key.begin(), fake::key.end(), [=](const auto& event) {
            return event.code == hop::MapMacKey(mac) && event.action == action;
        });
    };
    auto mouseCount = [](int action, int button) {
        return std::count_if(fake::mouse.begin(), fake::mouse.end(), [=](const auto& event) {
            return event.action == action && event.button == button;
        });
    };
    auto axisCount = [](int action) {
        return std::count_if(fake::mouse.begin(), fake::mouse.end(), [=](const auto& event) { return event.action == action; });
    };
    auto axisValue = [](int axis, float value) {
        return std::any_of(fake::mouse.begin(), fake::mouse.end(), [=](const auto& event) {
            return event.action == MOUSE_ACTION_AXIS_UPDATE && event.axis == axis && event.value == value;
        });
    };
    const bool ok = persisted == 1 && local == 2 && remote == 1 && leaving == 1 && errors == 0 &&
        fake::Empty() && fake::caps == 0 && fake::created == fake::destroyed &&
        keyCount(0, KEY_ACTION_DOWN) == 2 && keyCount(0, KEY_ACTION_UP) == 1 && // A repeat
        keyCount(62, KEY_ACTION_DOWN) == 1 && keyCount(62, KEY_ACTION_UP) == 1 && // right Control
        keyCount(55, KEY_ACTION_DOWN) == 1 && keyCount(55, KEY_ACTION_UP) == 1 && // held Meta
        keyCount(11, KEY_ACTION_DOWN) == 1 && keyCount(11, KEY_ACTION_UP) == 1 && // held B
        keyCount(57, KEY_ACTION_DOWN) == 2 && keyCount(57, KEY_ACTION_UP) == 2 && // restore Caps
        mouseCount(MOUSE_ACTION_BUTTON_DOWN, MOUSE_BUTTON_LEFT) == 2 &&
        mouseCount(MOUSE_ACTION_BUTTON_UP, MOUSE_BUTTON_LEFT) == 2 &&
        mouseCount(MOUSE_ACTION_BUTTON_DOWN, MOUSE_BUTTON_RIGHT) == 1 &&
        mouseCount(MOUSE_ACTION_BUTTON_UP, MOUSE_BUTTON_RIGHT) == 1 &&
        mouseCount(MOUSE_ACTION_MOVE, MOUSE_BUTTON_LEFT) >= 1 &&
        axisCount(MOUSE_ACTION_AXIS_BEGIN) == 2 && axisCount(MOUSE_ACTION_AXIS_UPDATE) == 2 &&
        axisCount(MOUSE_ACTION_AXIS_END) == 2 && axisValue(0, -2.0f) && axisValue(1, -1.0f);
    log(ok ? "PASS_RECORDED_INPUT_AND_RELEASE" : "FAIL");
    return ok ? 0 : 1;
}
