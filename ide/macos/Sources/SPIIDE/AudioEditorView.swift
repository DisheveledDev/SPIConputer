import SwiftUI

import SPIIDECore

/// Editor for an `audio` component: instruments and score timelines.
struct AudioEditorView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                soundsSection(sounds: $model.audioAsset.sounds)
                scoresSection(scores: $model.audioAsset.scores)
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onChange(of: model.audioAsset) {
            model.scheduleSave()
        }
    }

    // MARK: Sounds

    private func soundsSection(sounds: Binding<[SoundDef]>) -> some View {
        GroupBox("Sounds") {
            VStack(alignment: .leading, spacing: 12) {
                if sounds.wrappedValue.isEmpty {
                    Text("No sounds yet. Add one to define an instrument.")
                        .foregroundStyle(.secondary)
                }
                ForEach(sounds.wrappedValue.indices, id: \.self) { index in
                    SoundEditor(sound: sounds[index]) {
                        sounds.wrappedValue.remove(at: index)
                    }
                    if index < sounds.wrappedValue.count - 1 {
                        Divider()
                    }
                }
                Button("Add Sound", systemImage: "plus") {
                    let nextID = (sounds.wrappedValue.map(\.id).max() ?? -1) + 1
                    sounds.wrappedValue.append(SoundDef(id: nextID))
                }
            }
            .padding(8)
        }
    }

    // MARK: Scores

    private func scoresSection(scores: Binding<[ScoreDef]>) -> some View {
        GroupBox("Scores") {
            VStack(alignment: .leading, spacing: 12) {
                if scores.wrappedValue.isEmpty {
                    Text("No scores yet. Add one to lay notes on a timeline.")
                        .foregroundStyle(.secondary)
                }
                ForEach(scores.wrappedValue.indices, id: \.self) { index in
                    ScoreEditor(score: scores[index]) {
                        scores.wrappedValue.remove(at: index)
                    }
                    if index < scores.wrappedValue.count - 1 {
                        Divider()
                    }
                }
                Button("Add Score", systemImage: "plus") {
                    scores.wrappedValue.append(ScoreDef())
                }
            }
            .padding(8)
        }
    }
}

private struct SoundEditor: View {
    @Binding var sound: SoundDef
    let onDelete: () -> Void

    var body: some View {
        Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 8) {
            GridRow {
                Text("Sound \(sound.id)").font(.headline)
                Spacer()
                Button("Delete Sound", systemImage: "trash", action: onDelete)
                    .labelStyle(.iconOnly)
                    .buttonStyle(.borderless)
            }
            GridRow {
                NumberField(label: "ID", value: $sound.id)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Wave").font(.caption).foregroundStyle(.secondary)
                    Picker("Wave", selection: $sound.wave) {
                        ForEach(Waveform.allCases, id: \.self) { wave in
                            Text(wave.displayName).tag(wave)
                        }
                    }
                    .labelsHidden()
                    .frame(width: 110)
                }
                NumberField(label: "Duty", value: $sound.duty)
                NumberField(label: "Volume", value: $sound.volume)
            }
            GridRow {
                NumberField(label: "Attack (ms)", value: $sound.attack)
                NumberField(label: "Decay (ms)", value: $sound.decay)
                NumberField(label: "Sustain", value: $sound.sustain)
                NumberField(label: "Release (ms)", value: $sound.release)
            }
        }
    }
}

private struct ScoreEditor: View {
    @Binding var score: ScoreDef
    let onDelete: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Name")
                TextField("Score name", text: $score.name)
                    .frame(width: 160)
                Toggle("Loop", isOn: $score.loop)
                Spacer()
                Button("Delete Score", systemImage: "trash", action: onDelete)
                    .labelStyle(.iconOnly)
                    .buttonStyle(.borderless)
            }
            ForEach(score.channels.indices, id: \.self) { channelIndex in
                ChannelEditor(channel: $score.channels[channelIndex]) {
                    score.channels.remove(at: channelIndex)
                }
            }
            Button("Add Channel", systemImage: "plus") {
                score.channels.append([])
            }
            .disabled(score.channels.count >= 8)
        }
    }
}

private struct ChannelEditor: View {
    @Binding var channel: [NoteEvent]
    let onDelete: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Label("Channel", systemImage: "waveform")
                    .font(.subheadline)
                Spacer()
                Button("Delete Channel", systemImage: "minus", action: onDelete)
                    .labelStyle(.iconOnly)
                    .buttonStyle(.borderless)
            }
            ForEach(channel.indices, id: \.self) { eventIndex in
                NoteEventRow(event: $channel[eventIndex]) {
                    channel.remove(at: eventIndex)
                }
            }
            Button("Add Note", systemImage: "plus") {
                let at = channel.map { $0.at + $0.dur }.max() ?? 0
                channel.append(NoteEvent(at: at))
            }
        }
        .padding(8)
        .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 6))
    }
}

private struct NoteEventRow: View {
    @Binding var event: NoteEvent
    let onDelete: () -> Void

    var body: some View {
        HStack(alignment: .bottom, spacing: 10) {
            NumberField(label: "At (ms)", value: $event.at, width: 56)
            NumberField(label: "Sound", value: $event.sound, width: 46)
            VStack(alignment: .leading, spacing: 2) {
                Text("Note").font(.caption).foregroundStyle(.secondary)
                TextField("C4", text: $event.note)
                    .frame(width: 52)
            }
            NumberField(label: "Dur (ms)", value: $event.dur, width: 56)
            NumberField(label: "Vol", value: $event.vol, width: 46)
            NumberField(label: "Pan", value: $event.pan, width: 46)
            Button("Delete Note", systemImage: "minus", action: onDelete)
                .labelStyle(.iconOnly)
                .buttonStyle(.borderless)
        }
    }
}

private struct NumberField: View {
    let label: String
    @Binding var value: Int
    var width: CGFloat = 50

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label).font(.caption).foregroundStyle(.secondary)
            TextField(label, value: $value, format: .number)
                .frame(width: width)
        }
    }
}
