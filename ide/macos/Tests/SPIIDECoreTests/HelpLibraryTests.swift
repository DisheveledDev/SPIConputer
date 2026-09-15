import Foundation
import Testing

@testable import SPIIDECore

@Suite("Help library")
struct HelpLibraryTests {
    @Test func loadsTheThreeDocuments() {
        let sources = HelpLibrary.documents.map(\.source).sorted()
        #expect(sources == ["lua", "os", "sdk"])
        #expect(HelpLibrary.entries.count > 300)
        for entry in HelpLibrary.entries {
            #expect(!entry.name.isEmpty && !entry.signature.isEmpty, "entry without name/signature")
            #expect(!entry.summary.isEmpty, "\(entry.name) has no summary")
            #expect(["function", "method", "namespace", "module", "constant", "keyword"].contains(entry.kind),
                    "\(entry.name) has kind \(entry.kind)")
        }
    }

    @Test func looksUpNamesMethodsAndNamespaces() throws {
        let outText = try #require(HelpLibrary.entry(named: "Screen.OutText"))
        #expect(outText.source == "sdk" && outText.framework == "screen")
        #expect(outText.parameters.map(\.name) == ["x", "y", "text", "attr"])
        #expect(outText.parameters[3].optional)

        // Methods: a file object's read, and a Timer object's Pause.
        #expect(HelpLibrary.entry(named: "f:read")?.name == ":read")
        #expect(HelpLibrary.entry(named: ":close")?.name == ":close")
        #expect(HelpLibrary.entry(named: "t:Pause")?.name == "Timer.Pause")
        #expect(HelpLibrary.entry(named: "nothing:Frobnicate") == nil)

        // Namespaces list their members, functions before constants.
        let timer = try #require(HelpLibrary.entry(named: "Timer"))
        #expect(timer.kind == "namespace")
        let members = HelpLibrary.members(of: "Timer").map(\.name)
        #expect(members.contains("Timer.After") && members.contains("Timer.Pause"))
        let screen = HelpLibrary.members(of: "Screen")
        #expect(screen.last?.kind == "constant")
        #expect(HelpLibrary.members(of: "Input.Keyboard").map(\.name).contains("Input.Keyboard.Callback"))

        // Lua and OS entries.
        #expect(HelpLibrary.entry(named: "string.format")?.source == "lua")
        #expect(HelpLibrary.entry(named: "for")?.kind == "keyword")
        #expect(HelpLibrary.entry(named: "fs.readall")?.source == "os")
        #expect(HelpLibrary.entry(named: "ScreenWrite")?.group == "Display")
    }

    @Test func searchRanksNameMatchesFirst() {
        let hits = HelpLibrary.search("center").map(\.name)
        #expect(hits.first == "Text.Center" || hits.first == "Screen.CenterText" || hits.first == "Overlay.CenterText")
        #expect(hits.contains("Screen.CenterText") && hits.contains("Text.Center"))
        #expect(HelpLibrary.search("").isEmpty)
        #expect(!HelpLibrary.search("timer").isEmpty)
    }

    @Test func everyOSAPIFunctionIsDocumented() {
        // Everything the completion tables offer must have help, so the
        // panel never comes up empty on a call the editor suggested.
        let names = Set(HelpLibrary.entries.map(\.name))
        var missing: [String] = []
        for name in LuaCompletion.spiComputer + LuaCompletion.builtins where !names.contains(name) {
            missing.append(name)
        }
        for name in LuaSignatures.table.keys where !names.contains(name) {
            missing.append(name)
        }
        #expect(missing.isEmpty, "undocumented API: \(missing.sorted())")
    }

    @Test func everyFrameworkFunctionIsDocumentedWithItsSignature() {
        // Framework entries are curated in help/sdk.json but must track the
        // `---` line above each function in Resources/sdk/*.lua.
        var problems: [String] = []
        for sdk in SDKLibrary.available {
            for block in sdk.blocks where !block.isLocal {
                guard let entry = HelpLibrary.entry(named: block.name) else {
                    problems.append("missing: \(block.name)")
                    continue
                }
                if entry.framework != sdk.id {
                    problems.append("\(block.name): framework \(entry.framework ?? "nil") != \(sdk.id)")
                }
                let expected = block.signature.display
                let actual = LuaSignature(name: entry.name, parameters: SDKLibrary.parseSignature(entry.signature, fallbackName: entry.name).parameters).display
                if expected != actual {
                    problems.append("\(block.name): help says \(actual), framework says \(expected)")
                }
            }
            #expect(HelpLibrary.entry(named: sdk.namespaces.first ?? "")?.kind == "namespace",
                    "\(sdk.id): namespace entry")
        }
        #expect(problems.isEmpty, "\(problems.joined(separator: "\n"))")
    }

    @Test func seeAlsoLinksResolve() {
        var broken: [String] = []
        for entry in HelpLibrary.entries {
            for link in entry.seeAlso where HelpLibrary.entry(named: link) == nil {
                broken.append("\(entry.name) -> \(link)")
            }
        }
        #expect(broken.isEmpty, "\(broken.joined(separator: ", "))")
    }
}
