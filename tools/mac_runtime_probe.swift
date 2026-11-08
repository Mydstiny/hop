import AppKit
import Foundation

// Read only the expected Hop application's public LaunchServices metadata.
// Never constructs AppController, NSApplication, input taps or a network client;
// does not query protected permissions, read Keychain or inspect other windows.
func fail(_ message: String, code: Int32) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(code)
}
guard CommandLine.arguments.count == 2 else {
    fail("Usage: mac_runtime_probe /absolute/path/Hop.app", code: 64)
}
let expected = URL(fileURLWithPath: CommandLine.arguments[1]).standardizedFileURL.resolvingSymlinksInPath()
guard let bundle = Bundle(url: expected), bundle.bundleIdentifier == "local.hop.macos" else {
    fail("Refusing to inspect a bundle other than local.hop.macos", code: 3)
}
let applications = NSRunningApplication.runningApplications(withBundleIdentifier: "local.hop.macos").filter {
    $0.bundleURL?.standardizedFileURL.resolvingSymlinksInPath() == expected && !$0.isTerminated
}
let records: [[String: Any]] = applications.map {
    ["pid": Int($0.processIdentifier), "activationPolicy": $0.activationPolicy.rawValue,
     "accessory": $0.activationPolicy == .accessory, "finishedLaunching": $0.isFinishedLaunching]
}
let result: [String: Any] = [
    "scope": "Only expected bundle path and public NSRunningApplication metadata; not window/input proof",
    "bundleIdentifier": "local.hop.macos", "bundlePath": expected.path,
    "running": !applications.isEmpty,
    "accessoryPolicyVerified": !applications.isEmpty && applications.allSatisfy { $0.activationPolicy == .accessory },
    "processes": records,
    "productLaunchedByProbe": false, "permissionQueryAttempted": false,
    "permissionGrantAttempted": false, "pairConfirmationAttempted": false,
    "captureEnableAttempted": false, "mainWindowAbsenceVerified": false,
    "runtimeInputVerified": false
]
do {
    let data = try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data("\n".utf8))
} catch { fail("Cannot serialize public application metadata", code: 1) }
exit(applications.isEmpty ? 2 : 0)
