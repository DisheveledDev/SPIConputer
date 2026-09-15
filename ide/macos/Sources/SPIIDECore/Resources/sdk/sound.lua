-- SDK: Sound
-- Summary: Built-in instruments and effects by name, tones and noise, music written as notes (MML), and ProTracker .mod files streamed from the card.
-- Namespaces: Sound, Music
--
-- Same format contract as screen.lua (preamble, `function Name.Sub`
-- blocks stripped when unused, `---` signature lines).
--
-- The OS has a bank of built-in sounds (like the ROM font): instruments
-- for tunes ("lead", "pulse", "bass", "piano", "organ", "strings",
-- "flute", "brass", "bell", "pluck", "chime", "kick", "snare", "hihat",
-- "tom", "clap") and effects for games ("coin", "jump", "laser", "zap",
-- "explosion", "hit", "hurt", "powerup", "blip", "select", "error",
-- "alarm", "engine", "splash", "bounce", "teleport"). Play them by name:
-- Sound.Effect("coin"), Sound.Play("piano", "E4", 300). An effect
-- carries its own pitch; an instrument plays the note you give. Every
-- built-in can be tweaked: Sound.Define(id, { base = "laser", slide =
-- -600 }) makes a new sound from it. A sound is a waveform, an ADSR
-- envelope and pitch/tone effects: slide (semitones per second),
-- vibrato (cents) with vibrato_rate (Hz), arp/arp2 (semitone steps every
-- arp_ms, arp_loop to cycle) and cutoff (255 open, lower = darker).
--
-- Music.Track writes a tune as notes in MML (Music Macro Language),
-- one string per channel, up to 8 channels playing together:
--   "T120 L8 O4 c d e f g a b > c"   notes with lengths, octave, tempo
-- See Music.Track for the notation. Music.LoadMod plays a ProTracker
-- .mod file from the card, streamed (short samples resident, long ones
-- read ahead), so a song of a few hundred KB costs about 60 KB of RAM.
-- Big modules with many long samples may not keep up on the card.
-- The engine mixes 8 tune channels
-- plus 8 effect voices, stereo, 44.1 kHz; the product board's audio
-- output is not wired yet, so on hardware these calls succeed silently
-- until it is (the simulator plays them).

Sound = Sound or {}
Music = Music or {}

-- Instrument ids this framework reserves at the top of the 0..31 range
-- for Tone and Noise, defined on first use.
local TONE_ID = 31
local NOISE_ID = 30
local tone_defined = false
local noise_defined = false

local function ensure_tone()
    if not tone_defined then
        SoundDefine(TONE_ID, { wave = "square", duty = 8, attack = 2, decay = 10,
                               sustain = 220, release = 30 })
        tone_defined = true
    end
end

local function ensure_noise()
    if not noise_defined then
        SoundDefine(NOISE_ID, { wave = "noise", attack = 1, decay = 40,
                                sustain = 120, release = 60 })
        noise_defined = true
    end
end

-- MIDI note number nearest to a frequency in Hz (A4 = 440 = 69).
local function note_for_hz(hz)
    if not hz or hz <= 0 then return 69 end
    local n = math.floor(69 + 12 * math.log(hz / 440, 2) + 0.5)
    if n < 0 then n = 0 end
    if n > 127 then n = 127 end
    return n
end

-- Semitone of each note letter within an octave.
local NOTE_SEMI = { c = 0, d = 2, e = 4, f = 5, g = 7, a = 9, b = 11 }

-- Compile one MML string into a channel's event list. State: tempo
-- (shared across channels, the last T wins), octave, default length,
-- volume 0..15 (starting at state.volume: the fewer the channels, the
-- louder each starts, so a full mix does not clip), gate 1..8. With `fixed_note` (a drum or effect's own pitch)
-- every note plays that pitch: "c c c" is three hits. Returns the
-- events and the channel's length in ms.
local function mml_channel(text, sound, state, fixed_note)
    local events = {}
    local pos, t = 1, 0
    local octave, length, volume, gate = 4, 4, state.volume, 7
    local repeat_stack = {}
    text = text:lower()

    local function number()
        local n = text:match("^%d+", pos)
        if n then pos = pos + #n return tonumber(n) end
        return nil
    end
    -- A length in beats (quarter = 1), with dots; nil when none given.
    local function duration(default_len)
        local n = number() or default_len
        local beats = 4 / n
        while text:sub(pos, pos) == "." do
            pos = pos + 1
            beats = beats * 1.5
        end
        return beats
    end
    local function ms(beats)
        return math.floor(beats * 60000 / state.tempo + 0.5)
    end

    while pos <= #text do
        local c = text:sub(pos, pos)
        pos = pos + 1
        if NOTE_SEMI[c] then
            local semi = NOTE_SEMI[c]
            local acc = text:sub(pos, pos)
            if acc == "#" or acc == "+" then semi = semi + 1 pos = pos + 1
            elseif acc == "-" then semi = semi - 1 pos = pos + 1 end
            local beats = duration(length)
            local note = fixed_note or (octave + 1) * 12 + semi
            local total = ms(beats)
            local hold = math.floor(total * gate / 8)
            -- A tie (&) joins the next note of the same pitch.
            while text:sub(pos, pos) == "&" do
                pos = pos + 1
                local nxt = text:sub(pos, pos)
                if NOTE_SEMI[nxt] then
                    pos = pos + 1
                    local a2 = text:sub(pos, pos)
                    if a2 == "#" or a2 == "+" or a2 == "-" then pos = pos + 1 end
                    local extra = ms(duration(length))
                    total = total + extra
                    hold = hold + extra
                else
                    break
                end
            end
            events[#events + 1] = { at = t, sound = sound, note = note, dur = hold,
                                    vol = math.floor(volume * 17) }
            t = t + total
        elseif c == "r" or c == "p" then
            t = t + ms(duration(length))
        elseif c == "o" then
            octave = number() or octave
        elseif c == ">" then
            octave = octave + 1
        elseif c == "<" then
            octave = octave - 1
        elseif c == "l" then
            length = number() or length
        elseif c == "t" then
            state.tempo = number() or state.tempo
        elseif c == "v" then
            volume = math.max(0, math.min(15, number() or volume))
        elseif c == "q" then
            gate = math.max(1, math.min(8, number() or gate))
        elseif c == "[" then
            repeat_stack[#repeat_stack + 1] = { start = pos, count = nil }
        elseif c == "]" then
            local rep = repeat_stack[#repeat_stack]
            if rep then
                if not rep.count then rep.count = (number() or 2) - 1 end
                if rep.count > 0 then
                    rep.count = rep.count - 1
                    pos = rep.start
                else
                    repeat_stack[#repeat_stack] = nil
                end
            end
        end
        -- Anything else (spaces, bar lines, newlines) is ignored.
    end
    return events, t
end

--- Sound.Tone(hz, ms [, volume])
-- Plays a square-wave tone at (nearest note to) `hz` for `ms`
-- milliseconds; `volume` 0..255 (default 255). Returns the voice id.
function Sound.Tone(hz, ms, volume)
    ensure_tone()
    return SoundPlay(TONE_ID, note_for_hz(hz), ms or 100, volume or 255)
end

--- Sound.Beep([ms])
-- A short 880 Hz beep (default 80 ms).
function Sound.Beep(ms)
    ensure_tone()
    return SoundPlay(TONE_ID, 81, ms or 80, 255)
end

--- Sound.Noise(ms [, volume])
-- A burst of noise for `ms` milliseconds (explosions, hits).
function Sound.Noise(ms, volume)
    ensure_noise()
    return SoundPlay(NOISE_ID, 60, ms or 200, volume or 255)
end

--- Sound.Effect(name [, volume [, pan]])
-- Plays a built-in effect at its own pitch: "coin", "jump", "laser",
-- "zap", "explosion", "hit", "hurt", "powerup", "blip", "select",
-- "error", "alarm", "engine", "splash", "bounce", "teleport" (the
-- instruments work too, at C4). Returns the voice id.
function Sound.Effect(name, volume, pan)
    return SoundPlay(name, nil, 0, volume or 255, pan or 0)
end

--- Sound.Play(sound [, note [, ms [, volume [, pan]]]])
-- Plays a sound: a built-in name ("piano", "laser"), an id from
-- Sound.Define or Sound.Load, at `note` (a MIDI number or a name such as
-- "C4"; default: the sound's own note); `ms` 0 or none means the sound's
-- own envelope; volume 0..255, pan -64..63. Returns the voice id.
function Sound.Play(sound, note, ms, volume, pan)
    return SoundPlay(sound, note, ms, volume, pan)
end

--- Sound.Instrument(name)
-- The id of a built-in sound, for scores and Music.Track channels.
function Sound.Instrument(name)
    local id = SoundPreset(name)
    return id
end

--- Sound.Presets([effects])
-- The built-in sounds: a list of { name, effect, id }, or with
-- `effects` true only the effects, false only the instruments.
function Sound.Presets(effects)
    local all = SoundPresets()
    if effects == nil then return all end
    local list = {}
    for _, p in ipairs(all) do
        if p.effect == effects then list[#list + 1] = p end
    end
    return list
end

--- Sound.Spec(name)
-- A built-in sound's definition as a spec table, to change and give to
-- Sound.Define (or use `base` there).
function Sound.Spec(name)
    local _, spec = SoundPreset(name)
    return spec
end

--- Sound.Define(id, spec)
-- Defines a sound of your own under id 0..29 (30 and 31 are Tone and
-- Noise): spec = { base = "built-in name" (start from it), wave =
-- "square"|"triangle"|"saw"|"sine"|"noise", duty = 1..15, attack, decay,
-- release = ms, sustain = 0..255, volume = 0..255, note = "C5", slide =
-- semitones per second, vibrato = cents, vibrato_rate = Hz, arp, arp2 =
-- semitone steps, arp_ms = ms per step, arp_loop = true to cycle,
-- cutoff = 0..255 (255 open, lower darker) }; every field is optional.
function Sound.Define(id, spec)
    return SoundDefine(id, spec)
end

--- Sound.Load(path [, root [, loop_start, loop_end]])
-- Loads a WAV file from the card (8/16-bit PCM, mono or stereo, up to
-- 48 kHz, 64 KB of samples a program) as an instrument: `root` is the
-- note the recording is of ("C2" for a bass sampled at C2; default C4),
-- and playing any other note shifts the pitch by the difference, so a
-- single recording covers the scale. `loop_start`/`loop_end` (frames)
-- sustain a note by repeating that stretch until it is released.
-- Returns a sound id for Sound.Play and Music.Track channels.
function Sound.Load(path, root, loop_start, loop_end)
    return SoundLoad(path, root, loop_start, loop_end)
end

--- Sound.Stop([voice])
-- Stops one voice returned by a Play call, or every one-shot voice.
function Sound.Stop(voice)
    if voice then
        return SoundStop(voice)
    end
    return SoundStopAll()
end

--- Sound.Volume(volume)
-- Sets the master volume 0..255.
function Sound.Volume(volume)
    return SoundVolume(volume)
end

--- Music.Track(name, spec)
-- Defines a tune from notes: spec = { tempo = 120, loop = true,
-- channels = { { sound = "lead", mml = "..." }, ... } } with up to 8
-- channels, each a built-in name or a sound id and an MML string:
--   c d e f g a b  notes (# or + sharp, - flat); a number after a note
--                  is its length (4 quarter, 8 eighth, 1 whole; a dot
--                  adds half), else the default length
--   r or p         a rest (with a length like a note)
--   l8             default length; o4 octave; > < octave up/down
--   t120           tempo in beats a minute; v0..v15 volume (each channel
--                  starts at 16 / sqrt(channels), capped at 15, so a full
--                  mix does not clip); q1..q8 how much of a note sounds
--                  (q8 legato, q4 staccato)
--   c4&c8          a tie joins two notes;  [ c d e ]3  repeats three times
-- Spaces and bar lines are ignored. On a drum or effect channel (a
-- sound with a pitch of its own, like "kick") every note is a hit at
-- that pitch, so "c c r c" is a rhythm. Returns true (and the compiled
-- score), or nil, err.
function Music.Track(name, spec)
    local n = math.max(1, #(spec.channels or {}))
    -- A starting volume that leaves the mix headroom: 15 for one
    -- channel, 11 for two, 8 for four, 5 for eight (a v in the MML
    -- overrides it).
    local state = { tempo = spec.tempo or 120,
                    volume = math.min(15, math.floor(16 / math.sqrt(n))) }
    local score = { loop = spec.loop and true or false, channels = {} }
    for i, ch in ipairs(spec.channels or {}) do
        if i > 8 then break end
        local sound = ch.sound or "lead"
        -- A drum or effect has a pitch of its own: its notes are hits.
        local own = type(sound) == "string" and SoundPreset(sound)
        local fixed = nil
        if own then
            local _, spec_ = SoundPreset(sound)
            fixed = spec_ and spec_.note
        end
        score.channels[i] = mml_channel(ch.mml or "", sound, state, fixed)
    end
    local ok, err = MusicDefine(name, score)
    if not ok then return nil, err end
    return true, score
end

--- Music.Define(name, spec)
-- Defines a score from events: { loop = bool, channels = { { { at = ms,
-- sound = name|id, note = "C4", dur = ms, vol = 0..255, pan = -64..63 },
-- ... }, ... } } (see lua.md). Music.Track is the easier way.
function Music.Define(name, spec)
    return MusicDefine(name, spec)
end

--- Music.Play(name [, loop])
-- Starts a defined tune; `loop` overrides the tune's own loop flag.
function Music.Play(name, loop)
    return MusicPlay(name, loop)
end

--- Music.Stop()
-- Stops the tune that is playing.
function Music.Stop()
    return MusicStop()
end

--- Music.Playing()
-- True while a tune is playing.
function Music.Playing()
    return MusicPlaying()
end

--- Music.LoadMod(path)
-- Loads a ProTracker module (.mod, 4 channels, 31 samples) from the
-- card: one per program; a second load replaces it. Samples up to the
-- resident budget stay in RAM, longer ones stream from the card as they
-- play. Returns a module object with Play, Stop, Playing, Position and
-- Info methods, or nil, err.
function Music.LoadMod(path)
    local ok, err = ModLoad(path)
    if not ok then return nil, err end
    return setmetatable({}, { __index = {
        Play = function(_, loop) return Music.PlayMod(loop) end,
        Stop = function() return Music.StopMod() end,
        Playing = function() return Music.ModPlaying() end,
        Position = function() return Music.ModPosition() end,
        Info = function() return Music.ModInfo() end,
    } })
end

--- Music.PlayMod([loop])
-- Plays the loaded module from the top, looping unless `loop` is false;
-- a playing tune stops.
function Music.PlayMod(loop)
    return ModPlay(loop)
end

--- Music.StopMod()
-- Stops the module.
function Music.StopMod()
    return ModStop()
end

--- Music.ModPlaying()
-- True while the module plays.
function Music.ModPlaying()
    return ModPlaying()
end

--- Music.ModPosition()
-- Where the module is: order, row, pattern (or nil): a game can sync to
-- the music (a new order every few seconds, a row several times a second).
function Music.ModPosition()
    return ModPosition()
end

--- Music.ModInfo()
-- The loaded module: { name, orders, patterns, samples, resident_kb,
-- underruns } (underruns counts frames a streamed sample was late for).
function Music.ModInfo()
    return ModInfo()
end
