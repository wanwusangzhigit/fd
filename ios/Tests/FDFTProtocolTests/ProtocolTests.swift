// ProtocolTests.swift — FDFT v2 round-trip tests, Swift port of test_protocol.cpp.
import XCTest
@testable import FDFTProtocol

final class ProtocolTests: XCTestCase {

    func testRoundtripFile() throws {
        let writer = ByteBufferWriter()
        try writeFileHeader(writer, name: "hello.txt", size: 5)
        try writeAll(writer, Array("hello".utf8))

        var reader = ByteBufferReader(writer.bytes)
        let kind = try readFramePrefix(&reader)
        XCTAssertEqual(kind, FDFT.kindFile)
        let header = try readFilePayload(&reader)
        XCTAssertEqual(header.name, "hello.txt")
        XCTAssertEqual(header.size, 5)
        let body = try readExact(&reader, 5)
        XCTAssertEqual(body, Array("hello".utf8))
    }

    func testRoundtripRegisterIP() throws {
        let writer = ByteBufferWriter()
        try writeRegisterIP(writer, ip: "192.168.1.10")
        var reader = ByteBufferReader(writer.bytes)
        let kind = try readFramePrefix(&reader)
        XCTAssertEqual(kind, FDFT.kindRegisterIP)
        let ip = try readRegisterIPPayload(&reader)
        XCTAssertEqual(ip, "192.168.1.10")
    }

    func testRoundtripHashTrailer() throws {
        let writer = ByteBufferWriter()
        let digest: [UInt8] = Array(repeating: 0xAB, count: 32)
        try writeHashTrailer(writer, algo: FDFT.hashAlgoSHA256, digest: digest)
        var reader = ByteBufferReader(writer.bytes)
        let kind = try readFramePrefix(&reader)
        XCTAssertEqual(kind, FDFT.kindHash)
        let (algo, gotDigest) = try readHashPayload(&reader)
        XCTAssertEqual(algo, FDFT.hashAlgoSHA256)
        XCTAssertEqual(gotDigest, digest)
    }

    func testBadMagic() {
        let writer = ByteBufferWriter()
        writer.append([0x58, 0x58, 0x58, 0x58, FDFT.version, FDFT.kindFile])
        var reader = ByteBufferReader(writer.bytes)
        XCTAssertThrowsError(try readFramePrefix(&reader)) { err in
            XCTAssertEqual(err as? ProtocolError, .invalidMagic)
        }
    }

    func testBadVersion() {
        let writer = ByteBufferWriter()
        writer.append([0x46, 0x44, 0x46, 0x54, 99, FDFT.kindFile])
        var reader = ByteBufferReader(writer.bytes)
        XCTAssertThrowsError(try readFramePrefix(&reader))
    }

    func testBadKind() {
        let writer = ByteBufferWriter()
        writer.append([0x46, 0x44, 0x46, 0x54, FDFT.version, 99])
        var reader = ByteBufferReader(writer.bytes)
        XCTAssertThrowsError(try readFramePrefix(&reader)) { err in
            XCTAssertEqual(err as? ProtocolError, .unknownKind(found: 99))
        }
    }

    func testEmptyNameThrows() {
        let writer = ByteBufferWriter()
        XCTAssertThrowsError(
            try writeFileHeader(writer, name: "", size: 0)
        ) { err in
            XCTAssertEqual(err as? ProtocolError, .nameLengthOutOfRange(found: 0))
        }
    }

    func testTooLargeNameThrows() {
        let writer = ByteBufferWriter()
        let huge = String(repeating: "x", count: FDFT.maxNameLen + 1)
        XCTAssertThrowsError(
            try writeFileHeader(writer, name: huge, size: 0)
        )
    }

    func testEofOnReadExact() {
        let writer = ByteBufferWriter()
        writer.append([1, 2, 3])
        var reader = ByteBufferReader(writer.bytes)
        XCTAssertThrowsError(try readExact(&reader, 8))
    }
}
