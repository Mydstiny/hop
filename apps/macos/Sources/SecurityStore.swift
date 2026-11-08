import Foundation
import Security

struct Binding: Codable, Equatable {
    let peerID: [UInt8]
    let token: [UInt8]
    let name: String
}

enum SecureStore {
    static let service = "local.hop.macos.pairing.v1"
    static var identity: [UInt8] {
        let defaults = UserDefaults.standard
        if let text = defaults.string(forKey: "HopIdentity"), let uuid = UUID(uuidString: text) { return bytes(uuid) }
        let uuid = UUID(); defaults.set(uuid.uuidString, forKey: "HopIdentity"); return bytes(uuid)
    }
    static func bytes(_ uuid: UUID) -> [UInt8] {
        var value = uuid.uuid
        return withUnsafeBytes(of: &value) { Array($0) }
    }
    static func random(_ count: Int) throws -> [UInt8] {
        var result = [UInt8](repeating: 0, count: count)
        guard SecRandomCopyBytes(kSecRandomDefault, count, &result) == errSecSuccess else {
            throw WireError.invalid("System secure random source unavailable")
        }
        return result
    }
    static func pairingCode() throws -> UInt32 {
        // Rejection sampling avoids modulo bias across the 900,000 valid codes.
        let bound: UInt32 = 900000
        let limit = UInt32.max - UInt32.max % bound
        while true {
            let n = try random(4).u32(0)
            if n < limit { return 100000 + n % bound }
        }
    }
    static func constantTimeEqual(_ a: [UInt8], _ b: [UInt8]) -> Bool {
        guard a.count == b.count else { return false }
        var difference: UInt8 = 0
        for i in a.indices { difference |= a[i] ^ b[i] }
        return difference == 0
    }
    static func load() throws -> Binding? {
        var item: CFTypeRef?
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: service, kSecAttrAccount as String: "peer",
                                    kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne]
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data else { throw WireError.invalid("Cannot read pairing Keychain (\(status))") }
        let result = try JSONDecoder().decode(Binding.self, from: data)
        guard result.peerID.count == 16, result.token.count == 32,
              result.peerID.contains(where: { $0 != 0 }), result.token.contains(where: { $0 != 0 }) else {
            throw WireError.invalid("Invalid Keychain binding; unpair before continuing")
        }
        return result
    }
    static func save(_ binding: Binding) throws {
        let data = try JSONEncoder().encode(binding)
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: service, kSecAttrAccount as String: "peer"]
        let update = SecItemUpdate(query as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        if update == errSecSuccess { return }
        guard update == errSecItemNotFound else { throw WireError.invalid("Keychain update failed (\(update))") }
        var add = query
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        let status = SecItemAdd(add as CFDictionary, nil)
        guard status == errSecSuccess else { throw WireError.invalid("Keychain save failed (\(status))") }
    }
    static func delete() throws {
        let status = SecItemDelete([kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: service, kSecAttrAccount as String: "peer"] as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw WireError.invalid("Keychain delete failed (\(status))") }
    }
}

struct Keymap: Decodable {
    struct Key: Decodable { let mac: UInt16; let mac_symbol: String; let harmony: Int?; let status: String }
    let version: Int
    let modifier_bits: [String: Int]
    let keys: [Key]
    static func load(url: URL) throws -> Keymap {
        let map = try JSONDecoder().decode(Keymap.self, from: Data(contentsOf: url))
        guard map.version == 1, Set(map.keys.map(\.mac)).count == map.keys.count,
              map.keys.allSatisfy({ $0.status == "unsupported" || $0.harmony != nil }),
              Set(map.modifier_bits.values) == Set(0...8) else { throw WireError.invalid("Invalid generated shared keymap") }
        return map
    }
    var supported: Set<UInt16> { Set(keys.filter { $0.status != "unsupported" }.map(\.mac)) }
    /// Derive modifier keycodes from the same canonical resource, never duplicate the physical map.
    var modifiers: [UInt16: UInt16] {
        let names = ["kVK_Shift": "left_shift", "kVK_RightShift": "right_shift", "kVK_Control": "left_ctrl",
                     "kVK_RightControl": "right_ctrl", "kVK_Option": "left_alt", "kVK_RightOption": "right_alt",
                     "kVK_Command": "left_meta", "kVK_RightCommand": "right_meta", "kVK_CapsLock": "caps_lock"]
        var result: [UInt16: UInt16] = [:]
        for key in keys { if let name = names[key.mac_symbol], let bit = modifier_bits[name] { result[key.mac] = 1 << bit } }
        return result
    }
    func code(symbol: String) -> UInt16? { keys.first(where: { $0.mac_symbol == symbol })?.mac }
}
