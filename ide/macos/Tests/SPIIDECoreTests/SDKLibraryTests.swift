import Foundation
import Testing

@testable import SPIIDECore

@Suite("SDK library")
struct SDKLibraryTests {
    private let sample = """
    -- SDK: Demo
    -- Summary: A framework for the tests.
    -- Namespaces: Demo, Demo.Sub

    Demo = Demo or {}
    Demo.Sub = Demo.Sub or {}
    Demo.LIMIT = 3          -- a constant
    Demo.Sub.NAME = "sub"
    local counter = 0

    local function helper(n)
        counter = counter + n
        return counter
    end

    --- Demo.One(x [, attr])
    -- Uses the helper.
    function Demo.One(x, attr)
        return helper(x)
    end

    --- Demo.Two(a, b [, c [, d]])
    -- Calls One.
    function Demo.Two(a, b, c, d)
        return Demo.One(a + b)
    end

    function Demo.Sub.Three()
        return 3
    end

    """

    @Test func parsesHeaderPreambleAndBlocks() {
        let sdk = SDKLibrary.parse(id: "demo", text: sample)
        #expect(sdk.title == "Demo")
        #expect(sdk.summary == "A framework for the tests.")
        #expect(sdk.namespaces == ["Demo", "Demo.Sub"])
        #expect(sdk.preamble.contains("Demo = Demo or {}"))
        #expect(sdk.preamble.contains("local counter = 0"))
        #expect(!sdk.preamble.contains("function"))
        #expect(sdk.constants == ["Demo.LIMIT", "Demo.Sub.NAME"])
        #expect(sdk.blocks.map(\.name) == ["helper", "Demo.One", "Demo.Two", "Demo.Sub.Three"])
        #expect(sdk.blocks[0].isLocal)
        #expect(!sdk.blocks[1].isLocal)
    }

    @Test func signaturesComeFromDocLinesWithOptionalParameters() {
        let sdk = SDKLibrary.parse(id: "demo", text: sample)
        let signatures = sdk.signatures
        #expect(signatures.map(\.name) == ["Demo.One", "Demo.Two", "Demo.Sub.Three"])
        #expect(signatures[0].parameters == ["x", "[attr]"])
        #expect(signatures[1].parameters == ["a", "b", "[c]", "[d]"])
        // No doc line: the definition's own parameter list.
        #expect(signatures[2].parameters == [])
        #expect(sdk.blocks[1].summary == "Uses the helper.")
    }

    @Test func stripsUnusedFunctionsKeepingDependencies() throws {
        let sdk = SDKLibrary.parse(id: "demo", text: sample)
        let text = try #require(SDKLibrary.emit(sdk, usedBy: ["function setup() Demo.Two(1, 2) end"]))
        #expect(text.contains("Demo = Demo or {}"))
        #expect(text.contains("function Demo.Two("))
        #expect(text.contains("function Demo.One("), "Two calls One")
        #expect(text.contains("local function helper("), "One calls helper")
        #expect(!text.contains("Demo.Sub.Three"))
        // Blocks keep file order, so helpers are defined before use.
        let helperIndex = try #require(text.range(of: "local function helper"))
        let oneIndex = try #require(text.range(of: "function Demo.One"))
        #expect(helperIndex.lowerBound < oneIndex.lowerBound)
    }

    @Test func emitsNothingWhenUnused() {
        let sdk = SDKLibrary.parse(id: "demo", text: sample)
        #expect(SDKLibrary.emit(sdk, usedBy: ["function setup() print('hi') end"]) == nil)
        // A mention inside a comment or a string does not count.
        #expect(SDKLibrary.emit(sdk, usedBy: ["-- Demo.One is nice\nlocal s = 'Demo.Two'"]) == nil)
    }

    @Test func nestedNamespacesAndLocalHelpersAreMatchedExactly() throws {
        let sdk = SDKLibrary.parse(id: "demo", text: sample)
        let text = try #require(SDKLibrary.emit(sdk, usedBy: ["x = Demo.Sub.Three()"]))
        #expect(text.contains("function Demo.Sub.Three"))
        #expect(!text.contains("function Demo.One"))
        #expect(!text.contains("local function helper"))
        // A user-defined `helper` does not pull in the SDK's local helper.
        #expect(SDKLibrary.emit(sdk, usedBy: ["local function helper() end helper()"]) == nil)
    }

    @Test func methodCallsKeepTheNamespaceFunction() throws {
        // `t:Pause()` on a Timer object dispatches to Timer.Pause, so a
        // program that only uses the method form still needs the block.
        let timer = try #require(SDKLibrary.sdk(id: "timer"))
        let text = try #require(SDKLibrary.emit(
            timer, usedBy: ["local t = Timer.Every(100, tick)\nt:Pause()\nt:Resume()"]))
        #expect(text.contains("function Timer.Every("))
        #expect(text.contains("function Timer.Create("), "Every calls Create")
        #expect(text.contains("function Timer.Pause("), "kept through t:Pause()")
        #expect(text.contains("function Timer.Resume("), "kept through t:Resume()")
        #expect(text.contains("local function arm("), "Create calls arm")
        #expect(text.contains("local function fire("), "arm calls fire")
        #expect(text.contains("local function resolve("), "Pause calls resolve")
        #expect(!text.contains("function Timer.CancelAll("))
        // A method name that matches no block keeps nothing extra.
        #expect(SDKLibrary.emit(timer, usedBy: ["obj:Frobnicate()"]) == nil)
        // Identifiers: a method token is recorded with its colon.
        let ids = SDKLibrary.identifiers(in: "x = a.b:Pause(1) -- t:Comment()")
        #expect(ids.contains(":Pause") && ids.contains("a.b") && !ids.contains(":Comment"))
    }

    @Test func bundledFrameworksLoadAndParse() throws {
        let ids = SDKLibrary.available.map(\.id)
        #expect(ids == ["screen", "overlay", "text", "timer", "sound", "input"])
        for sdk in SDKLibrary.available {
            #expect(!sdk.blocks.isEmpty, "\(sdk.id) has functions")
            #expect(!sdk.summary.isEmpty, "\(sdk.id) has a summary")
            for block in sdk.blocks where !block.isLocal {
                #expect(block.name.contains("."), "\(block.name) is namespaced")
                #expect(block.signature.name == block.name, "\(block.name) doc line names the block")
            }
        }
        let screen = try #require(SDKLibrary.sdk(id: "screen"))
        let outText = try #require(screen.signatures.first { $0.name == "Screen.OutText" })
        #expect(outText.parameters == ["x", "y", "text", "[attr]"])
        let input = try #require(SDKLibrary.sdk(id: "input"))
        #expect(input.signatures.contains { $0.name == "Input.Keyboard.Callback" })
        #expect(input.signatures.contains { $0.name == "Input.Joystick.Callback" })
    }

    @Test func everyBundledBlockCompilesOnItsOwn() throws {
        // Each stripped subset must be valid Lua: emit every framework
        // with exactly one public function used, and check the syntax of
        // the whole (preamble + closure) with the tokenizer-based checker.
        for sdk in SDKLibrary.available {
            for block in sdk.blocks where !block.isLocal {
                let text = try #require(SDKLibrary.emit(sdk, referenced: [block.name]))
                let issue = LuaStructureChecker.check(text)
                #expect(issue == nil, "\(block.name): \(String(describing: issue))")
            }
        }
    }

    @Test func constantsAloneKeepThePreamble() {
        let sdk = SDKLibrary.parse(id: "demo", text: sample)
        let text = SDKLibrary.emit(sdk, usedBy: ["local n = Demo.LIMIT * 2"])
        #expect(text?.contains("Demo.LIMIT = 3") == true)
        #expect(text?.contains("function Demo.One") == false)
        #expect(SDKLibrary.emit(sdk, usedBy: ["print(1)"]) == nil)
    }

    @Test func screenFrameworkDefinesTheAttributeConstants() {
        let constants = SDKLibrary.constants(for: ["screen"])
        for name in ["Attributes.Inverse", "Attributes.Red", "Attributes.Blue", "Screen.INVERT", "Screen.COLS"] {
            #expect(constants.contains(name), "\(name)")
        }
        #expect(!constants.contains("Screen"), "namespace table is not a constant")
    }
}
