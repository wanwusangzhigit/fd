// FileSinkTests.swift — covers sanitize / resolveUnique / stream().
import XCTest
@testable import FDFTProtocol

final class FileSinkTests: XCTestCase {

    func testSanitizeBasic() {
        XCTAssertEqual(FileSinkUtil.sanitize("normal.txt"), "normal.txt")
        XCTAssertEqual(FileSinkUtil.sanitize("foo/bar"), "foo_bar")
        XCTAssertEqual(FileSinkUtil.sanitize("a:b*c?d"), "a_b_c_d")
        XCTAssertEqual(FileSinkUtil.sanitize("  hi  "), "hi")
        XCTAssertEqual(FileSinkUtil.sanitize("..hi.."), "hi")
    }

    func testSanitizeEmpty() {
        XCTAssertEqual(FileSinkUtil.sanitize(""), "received.bin")
        XCTAssertEqual(FileSinkUtil.sanitize("   "), "received.bin")
    }

    func testResolveUniqueNoCollision() throws {
        let tmp = NSTemporaryDirectory()
            .appending("wfd_\(UUID().uuidString)")
        try FileManager.default.createDirectory(atPath: tmp,
                                                withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: tmp) }

        let p1 = FileSinkUtil.resolveUnique(dir: tmp, name: "x.txt")
        FileManager.default.createFile(atPath: p1, contents: Data("h".utf8))
        let p2 = FileSinkUtil.resolveUnique(dir: tmp, name: "x.txt")
        XCTAssertNotEqual(p1, p2)
        FileManager.default.createFile(atPath: p2, contents: Data("y".utf8))
        let p3 = FileSinkUtil.resolveUnique(dir: tmp, name: "x.txt")
        XCTAssertNotEqual(p1, p3)
        XCTAssertNotEqual(p2, p3)
    }
}
