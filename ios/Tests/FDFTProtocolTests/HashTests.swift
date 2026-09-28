// HashTests.swift — SHA-256 known-answer tests (FIPS 180-4).
import XCTest
@testable import FDFTProtocol

final class HashTests: XCTestCase {

    func testKnownAnswers() {
        XCTAssertEqual(
            HashUtil.sha256Hex(Data()),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        )
        XCTAssertEqual(
            HashUtil.sha256Hex(Data("a".utf8)),
            "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"
        )
        XCTAssertEqual(
            HashUtil.sha256Hex(Data("abc".utf8)),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        )
        XCTAssertEqual(
            HashUtil.sha256Hex(Data("message digest".utf8)),
            "f7846f55cf23e14eebeab5b4e1550cad5b509e3348fbc4efa3a1413d393cb650"
        )
    }

    func testKnownAnswerLargeInput() {
        // SHA-256 of one million 'a' characters.
        let chunk = Data(repeating: 0x61, count: 1000)
        let stream = SHA256Stream()
        for _ in 0..<1000 { stream.update(chunk) }
        XCTAssertEqual(stream.finishHex(),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")
    }

    func testStreamingMatchesOneShot() {
        let n = 12345
        var data = Data(count: n)
        for i in 0..<n { data[i] = UInt8(i & 0xFF) }
        let oneShot = HashUtil.sha256Hex(data)

        let stream = SHA256Stream()
        for off in stride(from: 0, to: n, by: 7) {
            let end = min(off + 7, n)
            stream.update(data.subdata(in: off..<end))
        }
        XCTAssertEqual(stream.finishHex(), oneShot)
    }
}
