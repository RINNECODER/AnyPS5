// Ed25519 release signer (CryptoKit Curve25519.Signing), compiled and driven by signing.py.
//   signer sign <private-key-file> <message-file>   -> base64 signature + "\n" on stdout
//   signer public <private-key-file>                -> base64 raw public key + "\n" on stdout
// The key file holds base64 of the raw 32-byte private key. Its contents are never printed.
import CryptoKit
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data(("signer: " + message + "\n").utf8))
    exit(2)
}

func privateKey(_ path: String) -> Curve25519.Signing.PrivateKey {
    guard let text = try? String(contentsOfFile: path, encoding: .utf8) else { fail("cannot read the private key file") }
    guard let raw = Data(base64Encoded: text.trimmingCharacters(in: .whitespacesAndNewlines)), raw.count == 32,
          let key = try? Curve25519.Signing.PrivateKey(rawRepresentation: raw) else {
        fail("private key file is not base64 of 32 raw bytes")
    }
    return key
}

let arguments = CommandLine.arguments
switch (arguments.count, arguments.count > 1 ? arguments[1] : "") {
case (4, "sign"):
    guard let message = FileManager.default.contents(atPath: arguments[3]) else { fail("cannot read the message file") }
    let key = privateKey(arguments[2])
    guard let signature = try? key.signature(for: message), signature.count == 64,
          key.publicKey.isValidSignature(signature, for: message) else { fail("signing failed") }
    print(signature.base64EncodedString())
case (3, "public"):
    print(privateKey(arguments[2]).publicKey.rawRepresentation.base64EncodedString())
default:
    fail("usage: signer sign <key-file> <message-file> | signer public <key-file>")
}
