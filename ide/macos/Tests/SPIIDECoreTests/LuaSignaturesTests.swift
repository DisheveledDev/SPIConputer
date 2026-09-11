import Foundation
import Testing

@testable import SPIIDECore

@Suite("Lua signatures")
struct LuaSignaturesTests {
    @Test func signatureTableCoversTheAPI() {
        #expect(LuaSignatures.signature(for: "ScreenOut")?.display
            == "ScreenOut(x, y, char, [attr])")
        #expect(LuaSignatures.signature(for: "ScreenMode")?.display == "ScreenMode(mode)")
        #expect(LuaSignatures.signature(for: "fs.writeall")?.display
            == "fs.writeall(path, data)")
        #expect(LuaSignatures.signature(for: ":read")?.display == ":read(n)")
        #expect(LuaSignatures.signature(for: ":seek")?.display == ":seek(offset, [whence])")
        #expect(LuaSignatures.signature(for: "ApplyAssets")?.display == "ApplyAssets()")
        #expect(LuaSignatures.signature(for: "noSuchFunction") == nil)
    }

    @Test func parameterIndexHandlesVariadics() {
        let print = LuaSignatures.signature(for: "print")!
        #expect(print.parameterIndex(forArgument: 0) == 0)
        #expect(print.parameterIndex(forArgument: 5) == 0)

        let launch = LuaSignatures.signature(for: "Launch")!
        #expect(launch.parameterIndex(forArgument: 0) == 0)
        #expect(launch.parameterIndex(forArgument: 1) == 1)
        #expect(launch.parameterIndex(forArgument: 2) == nil)

        let soundPlay = LuaSignatures.signature(for: "SoundPlay")!
        #expect(soundPlay.parameterIndex(forArgument: 4) == 4)
        #expect(soundPlay.parameterIndex(forArgument: 5) == nil)
    }

    private func context(_ text: String, caret: Int? = nil) -> LuaSignatureHelp.Context? {
        LuaSignatureHelp.context(at: caret ?? (text as NSString).length, in: text)
    }

    @Test func findsCallAndActiveParameter() {
        #expect(context("ScreenOut(")?.signature.name == "ScreenOut")
        #expect(context("ScreenOut(")?.activeParameter == 0)
        #expect(context("ScreenOut(1, ")?.activeParameter == 1)
        #expect(context("ScreenOut(1, 2, 65, ")?.activeParameter == 3)
        // More arguments than the signature declares: no highlight.
        #expect(context("ScreenOut(1, 2, 65, 0x80, ")?.activeParameter == nil)
    }

    @Test func nestedCallsAndTables() {
        #expect(context("ScreenOut(string.byte(")?.signature.name == "string.byte")
        #expect(context("ScreenOut(string.byte(s, ")?.activeParameter == 1)
        #expect(context("ScreenOut(1, string.byte(s), ")?.signature.name == "ScreenOut")
        #expect(context("ScreenOut(1, string.byte(s), ")?.activeParameter == 2)
        // Commas inside a table constructor belong to the table.
        #expect(context("ScreenOut({a = 1, ")?.activeParameter == 0)
        #expect(context("ScreenOut({a = 1}, ")?.activeParameter == 1)
    }

    @Test func ignoresStringsAndComments() {
        #expect(context("ScreenOut(1, \"a,b\", ")?.activeParameter == 2)
        #expect(context("ScreenOut(1, -- c,\n 2, ")?.activeParameter == 2)
        #expect(context("-- ScreenOut(") == nil)
        #expect(context("local s = \"(\"\nlocal x = ") == nil)
    }

    @Test func stopsOutsideCalls() {
        #expect(context("local x = 1") == nil)
        #expect(context("ScreenOut(1)\nlocal x = ") == nil)
        #expect(context("unknownFunction(") == nil)
        #expect(context("myTable.helper(") == nil)
    }

    @Test func methodCallsUseTheMethodSignature() {
        #expect(context("f:read(")?.signature.display == ":read(n)")
        let text = "local f = fs.open(p)\nf:seek(o, "
        #expect(context(text)?.signature.name == ":seek")
        #expect(context(text)?.activeParameter == 1)
    }

    @Test func multiLineCalls() {
        let text = "ScreenOut(\n    1,\n    "
        #expect(context(text)?.signature.name == "ScreenOut")
        #expect(context(text)?.activeParameter == 1)
    }

    @Test func caretInTheMiddleOfAFullCall() {
        let text = "ScreenOut(1, 2, 65)"
        // Caret after "2, " (offset 15).
        #expect(context(text, caret: 15)?.activeParameter == 2)
        // Caret before the open paren: nothing.
        #expect(context(text, caret: 8) == nil)
    }
}
