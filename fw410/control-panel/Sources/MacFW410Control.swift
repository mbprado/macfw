import SwiftUI
import Foundation

private enum FW410Paths {
    static let controlTool = ProcessInfo.processInfo.environment["MACFW_FW410CTL"] ??
        "/Library/Application Support/macfw/fw410/tools/control/fw410ctl/fw410ctl"
    static let statusTool = ProcessInfo.processInfo.environment["MACFW_TRANSPORTSTATUS"] ??
        "/Library/Application Support/macfw/fw410/tools/transport/transportstatus/transportstatus"
}

private struct CommandResult {
    let status: Int32
    let output: String
}

private func runCommand(_ executable: String, _ arguments: [String]) -> CommandResult {
    guard FileManager.default.isExecutableFile(atPath: executable) else {
        return CommandResult(status: 127, output: "missing executable: \(executable)")
    }

    let process = Process()
    let pipe = Pipe()
    process.executableURL = URL(fileURLWithPath: executable)
    process.arguments = arguments
    process.standardOutput = pipe
    process.standardError = pipe

    do {
        try process.run()
        process.waitUntilExit()
    } catch {
        return CommandResult(status: 126, output: error.localizedDescription)
    }

    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    let output = String(data: data, encoding: .utf8)?
        .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    return CommandResult(status: process.terminationStatus, output: output)
}

@MainActor
final class FW410ControlModel: ObservableObject {
    static let mixerLabels = ["1/2", "3/4", "5/6", "7/8", "9/10"]

    @Published var connected = false
    @Published var transportState = "Unknown"
    @Published var activeRate = "—"
    @Published var headphoneSource = "mixer"
    @Published var headphoneLeft = 0.0
    @Published var headphoneRight = 0.0
    @Published var mixerState = Dictionary(uniqueKeysWithValues: mixerLabels.map { ($0, false) })
    @Published var lastError: String?
    @Published var refreshing = false

    func refresh() {
        guard !refreshing else { return }
        refreshing = true

        Task.detached(priority: .userInitiated) {
            let source = runCommand(FW410Paths.controlTool, ["headphone-source", "get"])
            let volume = runCommand(FW410Paths.controlTool, ["headphone-volume", "get"])
            let mixer = runCommand(FW410Paths.controlTool, ["headphone-mixer", "get"])
            let status = runCommand(FW410Paths.statusTool, [])

            await MainActor.run {
                self.applyRefresh(source: source, volume: volume, mixer: mixer, status: status)
                self.refreshing = false
            }
        }
    }

    func setSource(_ source: String) {
        Task.detached(priority: .userInitiated) {
            let result = runCommand(FW410Paths.controlTool, ["headphone-source", "set", source])
            await MainActor.run {
                if result.status == 0 {
                    self.headphoneSource = source
                    self.connected = true
                    self.lastError = nil
                } else {
                    self.lastError = result.output
                }
            }
        }
    }

    func setHeadphoneVolume() {
        let left = Int(headphoneLeft.rounded())
        let right = Int(headphoneRight.rounded())
        Task.detached(priority: .userInitiated) {
            let result = runCommand(FW410Paths.controlTool,
                                    ["headphone-volume", "set", "\(left)", "\(right)"])
            await MainActor.run {
                if result.status == 0 {
                    self.lastError = nil
                } else {
                    self.lastError = result.output
                    self.refresh()
                }
            }
        }
    }

    func setMixer(_ label: String, enabled: Bool) {
        let state = enabled ? "on" : "off"
        Task.detached(priority: .userInitiated) {
            let result = runCommand(FW410Paths.controlTool,
                                    ["headphone-mixer", "set", label, state])
            await MainActor.run {
                if result.status == 0 {
                    self.mixerState[label] = enabled
                    self.lastError = nil
                } else {
                    self.lastError = result.output
                    self.refresh()
                }
            }
        }
    }

    private func applyRefresh(source: CommandResult,
                              volume: CommandResult,
                              mixer: CommandResult,
                              status: CommandResult) {
        connected = source.status == 0
        lastError = connected ? nil : source.output

        if source.status == 0 {
            if source.output.hasPrefix("aux") {
                headphoneSource = "aux"
            } else if source.output.hasPrefix("mixer") {
                headphoneSource = "mixer"
            }
        }

        if volume.status == 0, let parsed = parseStereoVolume(volume.output) {
            headphoneLeft = parsed.0
            headphoneRight = parsed.1
        }

        if mixer.status == 0 {
            for line in mixer.output.split(separator: "\n") {
                let parts = line.split(separator: ":", maxSplits: 1).map {
                    $0.trimmingCharacters(in: .whitespaces)
                }
                if parts.count == 2, Self.mixerLabels.contains(parts[0]) {
                    mixerState[parts[0]] = parts[1] == "on"
                }
            }
        }

        if status.status == 0 {
            transportState = value(after: "transport state:", in: status.output) ?? "Unknown"
            if let rate = value(after: "active rate:", in: status.output) {
                activeRate = rate
            }
        } else {
            transportState = connected ? "Control online" : "Offline"
            activeRate = "—"
        }
    }

    private func parseStereoVolume(_ output: String) -> (Double, Double)? {
        var left: Double?
        var right: Double?
        for line in output.split(separator: "\n") {
            let text = String(line)
            if text.hasPrefix("left:") {
                left = parseDb(text)
            } else if text.hasPrefix("right:") {
                right = parseDb(text)
            }
        }
        if let left = left, let right = right { return (left, right) }
        return nil
    }

    private func parseDb(_ line: String) -> Double? {
        if line.contains("-inf") { return -128 }
        let components = line.split(whereSeparator: { $0 == " " || $0 == "\t" })
        guard components.count >= 2 else { return nil }
        return Double(components[1])
    }

    private func value(after prefix: String, in output: String) -> String? {
        for line in output.split(separator: "\n") {
            let text = String(line).trimmingCharacters(in: .whitespaces)
            if text.hasPrefix(prefix) {
                return String(text.dropFirst(prefix.count)).trimmingCharacters(in: .whitespaces)
            }
        }
        return nil
    }
}

struct LevelSlider: View {
    let title: String
    @Binding var value: Double
    let onCommit: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Text(title)
                .frame(width: 18, alignment: .leading)
                .foregroundColor(.secondary)
            Slider(value: $value, in: -128...0, step: 1, onEditingChanged: { editing in
                if !editing { onCommit() }
            })
            Text(value <= -128 ? "−∞" : "\(Int(value)) dB")
                .monospacedDigit()
                .frame(width: 58, alignment: .trailing)
        }
    }
}

struct ContentView: View {
    @StateObject private var model = FW410ControlModel()

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    sourceSection
                    volumeSection
                    mixerSection
                    if let error = model.lastError, !error.isEmpty {
                        errorSection(error)
                    }
                }
                .padding(24)
            }
        }
        .frame(minWidth: 520, idealWidth: 560, minHeight: 500, idealHeight: 540)
        .onAppear { model.refresh() }
    }

    private var header: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 3) {
                Text("M-Audio FireWire 410")
                    .font(.title2.weight(.semibold))
                HStack(spacing: 6) {
                    Circle()
                        .fill(model.connected ? Color.green : Color.secondary)
                        .frame(width: 8, height: 8)
                    Text(model.connected ? "Connected" : "Unavailable")
                        .foregroundColor(.secondary)
                    Text("•").foregroundColor(.secondary)
                    Text(model.transportState).foregroundColor(.secondary)
                    if model.activeRate != "—" {
                        Text("•").foregroundColor(.secondary)
                        Text(model.activeRate).foregroundColor(.secondary)
                    }
                }
                .font(.callout)
            }
            Spacer()
            Button(action: { model.refresh() }) {
                Image(systemName: "arrow.clockwise")
            }
            .buttonStyle(.borderless)
            .help("Refresh controls")
            .disabled(model.refreshing)
        }
        .padding(.horizontal, 24)
        .padding(.vertical, 18)
    }

    private var sourceSection: some View {
        GroupBox(label: Text("Headphone Source")) {
            Picker("Source", selection: Binding(
                get: { model.headphoneSource },
                set: { model.setSource($0) }
            )) {
                Text("Mixer").tag("mixer")
                Text("Auxiliary").tag("aux")
            }
            .pickerStyle(SegmentedPickerStyle())
            .labelsHidden()
            .padding(.vertical, 6)
        }
    }

    private var volumeSection: some View {
        GroupBox(label: Text("Headphone Level")) {
            VStack(spacing: 10) {
                LevelSlider(title: "L", value: $model.headphoneLeft) {
                    model.setHeadphoneVolume()
                }
                LevelSlider(title: "R", value: $model.headphoneRight) {
                    model.setHeadphoneVolume()
                }
            }
            .padding(.vertical, 8)
        }
    }

    private var mixerSection: some View {
        GroupBox(label: Text("Mixer Sources")) {
            VStack(alignment: .leading, spacing: 10) {
                Text("Select which mixer output pairs feed the headphones when Mixer is selected.")
                    .font(.callout)
                    .foregroundColor(.secondary)
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 85), spacing: 12)], spacing: 12) {
                    ForEach(FW410ControlModel.mixerLabels, id: \.self) { label in
                        Toggle(label, isOn: Binding(
                            get: { model.mixerState[label] ?? false },
                            set: { model.setMixer(label, enabled: $0) }
                        ))
                        .toggleStyle(SwitchToggleStyle())
                    }
                }
            }
            .padding(.vertical, 8)
        }
        .disabled(model.headphoneSource != "mixer")
        .opacity(model.headphoneSource == "mixer" ? 1 : 0.55)
    }

    private func errorSection(_ error: String) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundColor(.orange)
            Text(error)
                .font(.callout)
            Spacer()
        }
        .padding(12)
        .background(Color.secondary.opacity(0.08))
        .cornerRadius(8)
    }
}

@main
struct MacFW410ControlApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .commands {
            CommandGroup(replacing: .newItem) { }
        }
    }
}
