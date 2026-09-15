import Foundation
import Testing

@testable import SPIIDECore

@Suite("Function index and completions")
struct CompletionTests {
    @Test func findsFunctionDefinitions() {
        let source = """
        local helper = function(a, b)
        end

        function DrawShip(x, y)
        end

        local function tick()
        end

        function obj:draw(colour)
        end
        """
        let functions = LuaFunctionIndex.functions(in: source)
        #expect(functions.map(\.name) == ["helper", "DrawShip", "tick", "obj:draw"])
        #expect(functions[0].parameters == ["a", "b"])
        #expect(functions[1].parameters == ["x", "y"])
        #expect(functions[2].parameters == [])
        #expect(functions[3].parameters == ["colour"])
    }

    @Test func findsDefinitionsWithTheNameOnItsOwnLine() {
        let source = "function spaced(\n    a,\n    b\n)\nend\n"
        let functions = LuaFunctionIndex.functions(in: source)
        #expect(functions.map(\.name) == ["spaced"])
        #expect(functions.first?.parameters == ["a", "b"])
    }

    @Test func ignoresIncompleteDefinitionsAndComments() {
        let source = """
        -- function notReal(a)
        local s = "function alsoNotReal(b)"
        function open(
        """
        #expect(LuaFunctionIndex.functions(in: source).isEmpty)
    }

    @Test func lastDefinitionWins() {
        let source = "function f(a)\nend\nfunction f(a, b)\nend\n"
        let functions = LuaFunctionIndex.functions(in: source)
        #expect(functions.count == 1)
        #expect(functions.first?.parameters == ["a", "b"])
    }

    @Test func variadicParametersAreKept() {
        let functions = LuaFunctionIndex.functions(in: "local function log(level, ...)\nend\n")
        #expect(functions.first?.parameters == ["level", "..."])
        #expect(functions.first?.variadic == true)
    }

    @Test func completionIncludesLocalFunctionsWithParameters() {
        let source = "function DrawShip(x, y)\nend\n"
        let items = LuaCompletion.items("DrawS", in: source)
        #expect(items == [CompletionItem(name: "DrawShip", detail: "(x, y)")])
    }

    @Test func completionAndHelpCoverFrameworkNamespaces() throws {
        let sdk = SDKLibrary.signatures(for: ["screen", "input"])
        // A dotted prefix matches the namespaced names, with parameters.
        let items = LuaCompletion.items("Screen.Ou", in: "", including: sdk)
        #expect(items.map(\.name) == [
            "Screen.Out", "Screen.OutLine", "Screen.OutText", "Screen.OutLines", "Screen.OutWrapped",
        ])
        let outText = try #require(items.first { $0.name == "Screen.OutText" })
        #expect(outText.detail == "(x, y, text, [attr])")
        // Nested namespaces too.
        // Constants complete as plain names, with no parameter list.
        let attrs = LuaCompletion.items("Attributes.", in: "", including: sdk,
                                        constants: SDKLibrary.constants(for: ["screen"]))
        #expect(attrs.map(\.name).contains("Attributes.Red"))
        #expect(attrs.map(\.name).contains("Attributes.Inverse"))
        #expect(attrs.first { $0.name == "Attributes.Red" }?.detail == nil)

        let nested = LuaCompletion.items("Input.Keyboard.C", in: "", including: sdk)
        #expect(nested.map(\.name) == ["Input.Keyboard.Callback"])
        // Parameter help inside a framework call.
        let source = "function setup()\n  Screen.CenterText(1, \"hi\")\nend\n"
        let caret = (source as NSString).range(of: "\"hi\"").location
        let context = try #require(LuaSignatureHelp.context(at: caret, in: source, including: sdk))
        #expect(context.signature.name == "Screen.CenterText")
        #expect(context.activeParameter == 1)
    }

    @Test func completionIncludesProjectFunctions() {
        let items = LuaCompletion.items(
            "Tick", in: "",
            including: [LuaSignature(name: "TickShip", parameters: ["count"])])
        #expect(items == [CompletionItem(name: "TickShip", detail: "(count)")])
    }

    @Test func localDefinitionsShadowBuiltins() {
        let source = "function print(x)\nend\n"
        #expect(LuaCompletion.items("print", in: source)
            == [CompletionItem(name: "print", detail: "(x)")])
        #expect(LuaCompletion.items("print", in: "").first?.detail == "(...)")
    }

    @Test func builtinsShowTheirParameters() {
        let item = LuaCompletion.items("ScreenOut", in: "").first
        #expect(item?.detail == "(x, y, char, [attr])")
        // Keywords and values have no signature to show.
        #expect(LuaCompletion.items("while", in: "").first?.detail == nil)
    }

    @Test func signatureHelpFindsLocalFunctions() {
        let text = "function draw(a, b)\nend\n\ndraw("
        let context = LuaSignatureHelp.context(at: (text as NSString).length, in: text)
        #expect(context?.signature.name == "draw")
        #expect(context?.signature.parameters == ["a", "b"])
    }

    @Test func signatureHelpFindsProjectFunctions() {
        let context = LuaSignatureHelp.context(
            at: 4, in: "foo(",
            including: [LuaSignature(name: "foo", parameters: ["x", "y"])])
        #expect(context?.signature.parameters == ["x", "y"])
    }
}
