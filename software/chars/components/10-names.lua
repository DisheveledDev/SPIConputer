-- Names of the ROM font's picture, box-drawing, block and symbol
-- glyphs (from system/tools/mkfont.py, which builds the font; the codes
-- follow CP437). Printable ASCII is named by the character itself;
-- anything else is blank until a program defines it with DefineTile.

local GLYPH_NAMES = {
    [1] = "white smiling face",
    [2] = "black smiling face",
    [3] = "heart",
    [4] = "diamond",
    [5] = "club",
    [6] = "spade",
    [7] = "bullet",
    [9] = "white circle",
    [13] = "eighth note",
    [14] = "beamed eighth notes",
    [16] = "right-pointing pointer",
    [17] = "left-pointing pointer",
    [18] = "up down arrow",
    [24] = "up arrow",
    [25] = "down arrow",
    [26] = "right arrow",
    [27] = "left arrow",
    [28] = "right angle",
    [29] = "left right arrow",
    [30] = "up-pointing triangle",
    [31] = "down-pointing triangle",
    [174] = "left guillemet",
    [175] = "right guillemet",
    [176] = "light shade",
    [177] = "medium shade",
    [178] = "dark shade",
    [179] = "box: light vert",
    [180] = "box: light vert and left",
    [181] = "box: vert single and left double",
    [182] = "box: vert double and left single",
    [183] = "box: down double and left single",
    [184] = "box: down single and left double",
    [185] = "box: double vert and left",
    [186] = "box: double vert",
    [187] = "box: double down and left",
    [188] = "box: double up and left",
    [189] = "box: up double and left single",
    [190] = "box: up single and left double",
    [191] = "box: light down and left",
    [192] = "box: light up and right",
    [193] = "box: light up and horiz",
    [194] = "box: light down and horiz",
    [195] = "box: light vert and right",
    [196] = "box: light horiz",
    [197] = "box: light vert and horiz",
    [198] = "box: vert single and right double",
    [199] = "box: vert double and right single",
    [200] = "box: double up and right",
    [201] = "box: double down and right",
    [202] = "box: double up and horiz",
    [203] = "box: double down and horiz",
    [204] = "box: double vert and right",
    [205] = "box: double horiz",
    [206] = "box: double vert and horiz",
    [207] = "box: up single and horiz double",
    [208] = "box: up double and horiz single",
    [209] = "box: down single and horiz double",
    [210] = "box: down double and horiz single",
    [211] = "box: up double and right single",
    [212] = "box: up single and right double",
    [213] = "box: down single and right double",
    [214] = "box: down double and right single",
    [215] = "box: vert double and horiz single",
    [216] = "box: vert single and horiz double",
    [217] = "box: light up and left",
    [218] = "box: light down and right",
    [219] = "full block",
    [220] = "lower half block",
    [221] = "left half block",
    [222] = "right half block",
    [223] = "upper half block",
    [240] = "identical to",
    [241] = "plus-minus",
    [242] = "greater-than or equal",
    [243] = "less-than or equal",
    [246] = "division",
    [247] = "almost equal",
    [248] = "degree",
    [249] = "bullet operator",
    [250] = "middle dot",
    [253] = "superscript two",
    [254] = "black square",
}

local ASCII_NAMES = { [32] = "space", [127] = "delete" }

local function glyph_name(code)
    local name = GLYPH_NAMES[code] or ASCII_NAMES[code]
    if name then return name end
    if code > 32 and code < 127 then
        local c = string.char(code)
        if c:match("%u") then return "capital " .. c end
        if c:match("%l") then return "small " .. c end
        if c:match("%d") then return "digit " .. c end
        return "'" .. c .. "'"
    end
    return "(blank: free for DefineTile)"
end
