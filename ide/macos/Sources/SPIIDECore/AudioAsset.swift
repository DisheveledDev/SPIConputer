import Foundation

/// A sound/instrument and score asset, edited in the IDE and compiled at
/// build time to `SoundDefine` / `MusicDefine` calls.
public struct AudioAsset: Codable, Sendable, Equatable {
    public var sounds: [SoundDef]
    public var scores: [ScoreDef]

    public init(sounds: [SoundDef] = [], scores: [ScoreDef] = []) {
        self.sounds = sounds
        self.scores = scores
    }

    enum CodingKeys: String, CodingKey {
        case sounds, scores
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        sounds = try c.decodeIfPresent([SoundDef].self, forKey: .sounds) ?? []
        scores = try c.decodeIfPresent([ScoreDef].self, forKey: .scores) ?? []
    }
}

/// Waveform choices matching the OS sound engine.
public enum Waveform: String, Codable, CaseIterable, Sendable {
    case square, pulse, triangle, saw, sine, noise

    public var displayName: String {
        switch self {
        case .square: "Square"
        case .pulse: "Pulse"
        case .triangle: "Triangle"
        case .saw: "Saw"
        case .sine: "Sine"
        case .noise: "Noise"
        }
    }
}

/// One instrument (`SoundDefine` spec). Field ranges follow `lua.md`.
public struct SoundDef: Codable, Sendable, Equatable {
    public var id: Int
    public var wave: Waveform
    public var duty: Int
    public var attack: Int
    public var decay: Int
    public var sustain: Int
    public var release: Int
    public var volume: Int

    public init(
        id: Int = 0,
        wave: Waveform = .square,
        duty: Int = 8,
        attack: Int = 0,
        decay: Int = 0,
        sustain: Int = 255,
        release: Int = 20,
        volume: Int = 255
    ) {
        self.id = id
        self.wave = wave
        self.duty = duty
        self.attack = attack
        self.decay = decay
        self.sustain = sustain
        self.release = release
        self.volume = volume
    }

    enum CodingKeys: String, CodingKey {
        case id, wave, duty, attack, decay, sustain, release, volume
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decodeIfPresent(Int.self, forKey: .id) ?? 0
        wave = try c.decodeIfPresent(Waveform.self, forKey: .wave) ?? .square
        duty = try c.decodeIfPresent(Int.self, forKey: .duty) ?? 8
        attack = try c.decodeIfPresent(Int.self, forKey: .attack) ?? 0
        decay = try c.decodeIfPresent(Int.self, forKey: .decay) ?? 0
        sustain = try c.decodeIfPresent(Int.self, forKey: .sustain) ?? 255
        release = try c.decodeIfPresent(Int.self, forKey: .release) ?? 20
        volume = try c.decodeIfPresent(Int.self, forKey: .volume) ?? 255
    }
}

/// One score (`MusicDefine` spec): up to 8 channels of note events.
public struct ScoreDef: Codable, Sendable, Equatable {
    public var name: String
    public var loop: Bool
    public var channels: [[NoteEvent]]

    public init(name: String = "score", loop: Bool = false, channels: [[NoteEvent]] = []) {
        self.name = name
        self.loop = loop
        self.channels = channels
    }

    enum CodingKeys: String, CodingKey {
        case name, loop, channels
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? "score"
        loop = try c.decodeIfPresent(Bool.self, forKey: .loop) ?? false
        channels = try c.decodeIfPresent([[NoteEvent]].self, forKey: .channels) ?? []
    }
}

/// One note event in a score channel.
public struct NoteEvent: Codable, Sendable, Equatable {
    public var at: Int
    public var sound: Int
    /// Note name ("C4", "A#3") or a MIDI number ("60").
    public var note: String
    public var dur: Int
    public var vol: Int
    public var pan: Int

    public init(
        at: Int = 0,
        sound: Int = 0,
        note: String = "C4",
        dur: Int = 200,
        vol: Int = 255,
        pan: Int = 0
    ) {
        self.at = at
        self.sound = sound
        self.note = note
        self.dur = dur
        self.vol = vol
        self.pan = pan
    }

    enum CodingKeys: String, CodingKey {
        case at, sound, note, dur, vol, pan
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        at = try c.decodeIfPresent(Int.self, forKey: .at) ?? 0
        sound = try c.decodeIfPresent(Int.self, forKey: .sound) ?? 0
        if let text = try? c.decode(String.self, forKey: .note) {
            note = text
        } else if let number = try? c.decode(Int.self, forKey: .note) {
            note = String(number)
        } else {
            note = "C4"
        }
        dur = try c.decodeIfPresent(Int.self, forKey: .dur) ?? 200
        vol = try c.decodeIfPresent(Int.self, forKey: .vol) ?? 255
        pan = try c.decodeIfPresent(Int.self, forKey: .pan) ?? 0
    }
}
