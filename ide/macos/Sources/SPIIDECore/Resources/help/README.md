# Help documents

The IDE's help panel (right-click a name in the editor, or search the
panel) reads every `*.json` file in this folder. The three shipped are:

- `lua.json` — Lua 5.5 keywords, base functions and the modules the OS
  opens for programs (string, table, math, coroutine, utf8, package).
- `os.json` — the SPIComputer OS API: program contract, system calls,
  display, sound, `fs`.
- `sdk.json` — the frameworks (`Screen`/`Attributes`, `Overlay`, `Text`,
  `Timer`, `Sound`/`Music`, `Input`): namespaces, constants and every
  function. Constants are the column-0 `Name.CONST = value` lines of a
  framework's preamble; each needs an entry of kind `constant`.

**This folder is the documentation of record for the IDE.** When you add
or change an API call, a framework function or a Lua facility that
programs can use, update the matching entry here in the same change:
`HelpLibraryTests` fails when an OS API function or a framework function
has no entry, or when a framework entry's signature no longer matches
the `---` line above the function in `Resources/sdk/*.lua`.

## Schema

```json
{
  "source": "os",                 // lua | os | sdk (any string; groups the panel's source badge)
  "title": "SPIComputer OS API",
  "entries": [
    {
      "name": "Screen.OutText",          // the name as written at a call site; methods start with ":" (":read")
      "kind": "function",                // function | method | namespace | module | constant | keyword
      "group": "Screen",                 // heading the entry is listed under
      "framework": "screen",             // sdk entries only: the framework id
      "signature": "Screen.OutText(x, y, text [, attr])",
      "summary": "One line, shown in lists and at the top of the panel.",
      "description": "As many sentences as it takes: behaviour, limits, gotchas.",
      "parameters": [
        { "name": "x", "type": "number", "optional": false, "description": "column 0-39" }
      ],
      "returns": "true, or nil, err",
      "example": "Screen.OutText(2, 5, \"Hello\", 0x02)",
      "seeAlso": ["Screen.CenterText", "ScreenWrite"]
    }
  ]
}
```

Every field except `name`, `kind` and `signature` may be empty or
omitted. `seeAlso` names other entries (any file); the panel links them.
Keep examples short and real: something a program would actually write.
