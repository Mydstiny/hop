// Test-only host driver for the ACTUAL HarmonyOS Client, linked to recording fakes.
// No system input library, NAPI, device API, disk token store, or physical capture.
#include "client.h"
#include <multimodalinput/oh_input_manager.h>
#include <atomic>
#include <iostream>
#include <mutex>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::mutex output;
    std::atomic<int> persisted{0}, ready{0}, errors{0};
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
            ++ready; log("READY");
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
    const bool ok = persisted == 1 && ready == 1 && errors == 0 && fake::Empty() &&
                    fake::created == fake::destroyed && fake::mouse.empty() && fake::key.empty();
    log(ok ? "PASS_NO_INPUT" : "FAIL");
    return ok ? 0 : 1;
}
