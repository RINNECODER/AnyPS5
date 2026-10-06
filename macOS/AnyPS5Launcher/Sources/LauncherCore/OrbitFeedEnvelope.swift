import Foundation
import CryptoKit

/// Independent Swift implementation of Orbit's published version-1 feed envelope.
/// Wire format and public key-id 1 parameter: backend/catalog_crypto.c and
/// backend/catalog_key.h in https://github.com/saawant12/orbit-store-ps5/releases/tag/v0.8.0.
/// This uses CryptoKit directly; no upstream C implementation is included.
enum OrbitFeedEnvelope {
    private static let maximumPlaintextSize = 8_388_608
    // Published wire parameter for key-id 1; this is shared by the Orbit clients.
    private static let key = SymmetricKey(data: [UInt8](arrayLiteral:
        0x21, 0x43, 0x26, 0x3b, 0x71, 0x6b, 0x06, 0xec,
        0x56, 0x5d, 0x04, 0x7a, 0x8c, 0x37, 0x0d, 0xa7,
        0xe5, 0xd9, 0x69, 0x09, 0x75, 0x74, 0x8e, 0x83,
        0x6e, 0xd9, 0x2c, 0x66, 0x43, 0x41, 0xf6, 0xac
    ))

    static func decode(_ envelope: Data) throws -> Data {
        let prefix: [UInt8] = [0x4f, 0x52, 0x42, 0x49, 0x54, 0x45, 0x4e, 0x43, 1, 1, 1, 0]
        guard envelope.count > 40, envelope.count <= maximumPlaintextSize + 40,
              Array(envelope.prefix(12)) == prefix else {
            throw LauncherError("Orbit catalogue envelope has an unsupported format or size.")
        }
        let header = envelope.prefix(24)
        let plaintext: Data
        do {
            let nonce = try AES.GCM.Nonce(data: header.suffix(12))
            let box = try AES.GCM.SealedBox(nonce: nonce,
                                          ciphertext: envelope.dropFirst(24).dropLast(16),
                                          tag: envelope.suffix(16))
            plaintext = try AES.GCM.open(box, using: key, authenticating: header)
        } catch {
            throw LauncherError("Orbit catalogue authentication failed.")
        }
        guard !plaintext.isEmpty, plaintext.count <= maximumPlaintextSize else {
            throw LauncherError("Orbit catalogue envelope has an unsupported format or size.")
        }
        return plaintext
    }
}
