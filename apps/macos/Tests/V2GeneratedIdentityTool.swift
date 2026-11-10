import Foundation
@main struct V2GeneratedIdentityTool {
    static func main() throws {
        let cert = try String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8)
        let key = try String(contentsOfFile: CommandLine.arguments[2], encoding: .utf8)
        guard V2LocalIdentity(certificatePEM: cert, privateKeyPEM: key).valid else {
            fputs("FAIL generated TLS identity is incompatible with Mac Keychain adapter\n",stderr); exit(1)
        }
        print("PASS generated TLS identity matches Mac Keychain adapter (temporary test identity)")
    }
}
