-- SDK: Sound
-- Summary: Tones, beeps and effects without defining instruments first; music playback.
-- Namespaces: Sound, Music
--
-- Same format contract as screen.lua (preamble, `function Name.Sub`
-- blocks stripped when unused, `---` signature lines).
--
-- The OS sound engine is complete and runs in the simulator; the
-- product board's audio output is not wired yet, so on hardware these
-- calls succeed silently until it is.

Sound = Sound or {}
Music = Music or {}

-- Instrument ids this framework reserves at the top of the 0..31 range,
-- defined on first use so programs that only use Sound.Play pay nothing.
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

--- Sound.Define(id, spec)
-- Defines an instrument 0..29 (30 and 31 are used by Tone and Noise):
-- spec = {wave="square"|"pulse"|"triangle"|"saw"|"sine"|"noise",
-- duty=1..15, attack=ms, decay=ms, sustain=0..255, release=ms, volume=0..255}.
function Sound.Define(id, spec)
    return SoundDefine(id, spec)
end

--- Sound.Load(path)
-- Loads a WAV file from the card into the sample pool; returns a sound
-- id usable with Sound.Play and in scores.
function Sound.Load(path)
    return SoundLoad(path)
end

--- Sound.Play(sound [, note [, ms [, volume [, pan]]]])
-- Plays an instrument or sample: `note` is a MIDI number or a name such
-- as "C4"; `ms` 0 means the instrument's own envelope; pan -64..63.
function Sound.Play(sound, note, ms, volume, pan)
    return SoundPlay(sound, note, ms, volume, pan)
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

--- Music.Define(name, spec)
-- Defines a score by name (see lua.md for the spec: channels of
-- {time, sound, note, length, volume, pan} events, `loop`, `length`).
function Music.Define(name, spec)
    return MusicDefine(name, spec)
end

--- Music.Play(name [, loop])
-- Starts a defined score; `loop` overrides the score's own loop flag.
function Music.Play(name, loop)
    return MusicPlay(name, loop)
end

--- Music.Stop()
-- Stops the score that is playing.
function Music.Stop()
    return MusicStop()
end

--- Music.Playing()
-- True while a score is playing.
function Music.Playing()
    return MusicPlaying()
end
