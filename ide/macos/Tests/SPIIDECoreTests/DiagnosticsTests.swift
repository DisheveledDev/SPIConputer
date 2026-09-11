import Foundation
import Testing

@testable import SPIIDECore

@Suite("Diagnostics and completions")
struct DiagnosticsTests {
    @Test func parsesLuaErrorWithLocation() {
        let parsed = LuaErrorParser.parse(
            "[string \"/tmp/x.lua\"]:12: unexpected symbol near '}'")
        #expect(parsed.line == 12)
        #expect(parsed.message == "unexpected symbol near '}'")
        #expect(parsed.contextLine == nil)
    }

    @Test func parsesBareLocation() {
        let parsed = LuaErrorParser.parse("main.lua:4: 'end' expected")
        #expect(parsed.line == 4)
        #expect(parsed.message == "'end' expected")
    }

    @Test func parsesMessageWithoutLine() {
        let parsed = LuaErrorParser.parse("compile failed")
        #expect(parsed.line == nil)
        #expect(parsed.message == "compile failed")
    }

    @Test func parsesUnclosedConstructContext() {
        let parsed = LuaErrorParser.parse(
            "[string \"/tmp/x.lua\"]:120: 'end' expected (to close 'function' at line 41) near '<eof>'")
        #expect(parsed.line == 120)
        #expect(parsed.contextLine == 41)
    }

    @Test func completionCoversLuaAndOS() {
        #expect(LuaCompletion.matches("Screen").contains("ScreenOut"))
        #expect(LuaCompletion.matches("MusicPlay").contains("MusicPlay"))
        #expect(LuaCompletion.matches("string.f").contains("string.format"))
        #expect(LuaCompletion.matches("fs.read").contains("fs.readall"))
        #expect(LuaCompletion.matches("func").contains("function"))
        #expect(LuaCompletion.matches("zzz").isEmpty)
    }

    @Test func tokenizerClassifiesLua() {
        let source = """
        -- a comment
        local n = 42
        local s = "text"
        --[[ long
        comment ]]
        function ScreenOut(x)
            print(string.format("%d", n))
        end
        """
        let tokens = LuaTokenizer.tokenize(source)
        func kinds(_ needle: String) -> [LuaTokenizer.Kind] {
            let range = (source as NSString).range(of: needle)
            return tokens.filter { $0.range.location == range.location }.map(\.kind)
        }
        #expect(kinds("-- a comment") == [.comment])
        #expect(kinds("--[[ long") == [.comment])
        #expect(kinds("42") == [.number])
        #expect(kinds("\"text\"") == [.string])
        #expect(kinds("local") == [.keyword])
        #expect(kinds("function") == [.keyword])
        #expect(kinds("ScreenOut") == [.function])
        #expect(kinds("print") == [.function])
        #expect(kinds("string.format") == [.function])
    }

    @Test func structureCheckerAcceptsBalancedCode() {
        let source = """
        local function f(x)
            if x then
                for i = 1, 3 do
                    while false do
                        repeat
                            x = x - 1
                        until x < 0
                    end
                end
            else
                return
            end
        end
        f(1)
        """
        #expect(LuaStructureChecker.check(source) == nil)
    }

    @Test func structureCheckerFindsMissingEnd() {
        let source = """
        local function f(x)
            if x then
                return
            end
        """
        let issue = LuaStructureChecker.check(source)
        #expect(issue?.line == 1)
        #expect(issue?.message == "missing 'end' to close 'function'")
    }

    @Test func structureCheckerFindsStrayEnd() {
        let source = "local x = 1\nend\n"
        let issue = LuaStructureChecker.check(source)
        #expect(issue?.line == 2)
        #expect(issue?.message.contains("unexpected 'end'") == true)
    }

    @Test func structureCheckerIgnoresStringsAndComments() {
        let source = """
        -- function f() end
        local s = "end until function"
        local t = [[
        if x then
        ]]
        print(s, t)
        """
        #expect(LuaStructureChecker.check(source) == nil)
    }

    @Test func structureCheckerReportsRepeatUntil() {
        let source = "repeat\n    x = x + 1\nend\n"
        let issue = LuaStructureChecker.check(source)
        #expect(issue?.line == 3)
        #expect(issue?.message.contains("'until'") == true)
    }
}
