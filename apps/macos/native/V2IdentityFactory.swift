import Foundation

@_silgen_name("hop_tls_generate_identity") private func tlsGenerateIdentity(_ cert: UnsafeMutablePointer<CChar>, _ certCapacity: Int,
    _ key: UnsafeMutablePointer<CChar>, _ keyCapacity: Int, _ fingerprint: UnsafeMutablePointer<CChar>, _ fingerprintCapacity: Int) -> Int32

/// Generates a new device identity in memory. The caller saves it in Keychain
/// only after checking that no identity exists; this function never replaces it.
enum V2IdentityFactory {
    static func generate() throws -> V2LocalIdentity {
        var cert = [CChar](repeating: 0, count: 8192), key = [CChar](repeating: 0, count: 8192), fingerprint = [CChar](repeating: 0, count: 65)
        defer { for i in key.indices { key[i] = 0 } }
        let status = cert.withUnsafeMutableBufferPointer { c in
            key.withUnsafeMutableBufferPointer { k in
                fingerprint.withUnsafeMutableBufferPointer { f in
                    tlsGenerateIdentity(c.baseAddress!,c.count,k.baseAddress!,k.count,f.baseAddress!,f.count)
                }
            }
        }
        guard status == 0, cert.contains(0), key.contains(0), fingerprint.last == 0 else { throw WireError.invalid("Cannot generate v2 identity") }
        let identity = V2LocalIdentity(certificatePEM: String(cString: cert), privateKeyPEM: String(cString: key))
        guard identity.valid, identity.fingerprint == String(cString: fingerprint) else { throw WireError.invalid("Generated v2 identity is invalid") }
        return identity
    }
}
