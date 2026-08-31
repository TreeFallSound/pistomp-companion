import AppKit
import CoreAudio
import Foundation

final class SettingsWindowController: NSWindowController {
    private struct Defaults {
        static let hostname = "pistomp.local"
        static let clock = ""
        static let network = ""
    }
    private var timingIsLive = false
    private let hostnameField = NSTextField()
    private let networkPopup = NSPopUpButton()
    private let clockPopup = NSPopUpButton()
    private let workgroupPopup = NSPopUpButton()
    private let jitterPopup = NSPopUpButton()
    // Pi tuning. Every one of these is pushed to /etc/default/jackbridge by
    // jackbridge-ctl before the pi service starts — the pi holds no setting of
    // its own, so a reimage costs no configuration and a measurement is
    // repeatable.
    private let netLatencyPopup = NSPopUpButton()
    private let netRingPopup = NSPopUpButton()
    private let rtNapiField = NSTextField()
    private let rtIrqField = NSTextField()
    private let cpuNetField = NSTextField()
    private let cpuDspField = NSTextField()
    private let governorPopup = NSPopUpButton()
    private let qdiscPopup = NSPopUpButton()
    private let eeePopup = NSPopUpButton()
    private let nicRingField = NSTextField()
    private let offloadPopup = NSPopUpButton()
    private let timingField = NSTextField(labelWithString: "No value")
    private let errorField = NSTextField(labelWithString: "")
    private var values: [String: Any] = [:]
    private var loadError: Error?

    init() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 1),
                              styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.title = "JackBridge Settings"
        window.isReleasedWhenClosed = false
        super.init(window: window)
        buildView()
        reload()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    func present() {
        reload()
        NSApp.activate(ignoringOtherApps: true)
        window?.center()
        window?.makeKeyAndOrderFront(nil)
    }

    private func buildView() {
        let content = NSView()
        window?.contentView = content

        hostnameField.placeholderString = Defaults.hostname
        hostnameField.translatesAutoresizingMaskIntoConstraints = false
        networkPopup.translatesAutoresizingMaskIntoConstraints = false
        clockPopup.translatesAutoresizingMaskIntoConstraints = false
        timingField.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        timingField.textColor = .secondaryLabelColor
        timingField.lineBreakMode = .byWordWrapping
        errorField.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        errorField.textColor = .secondaryLabelColor
        errorField.lineBreakMode = .byWordWrapping

        let connection = NSStackView()
        connection.orientation = .vertical
        connection.alignment = .leading
        connection.spacing = 8
        connection.translatesAutoresizingMaskIntoConstraints = false
        connection.addArrangedSubview(sectionTitle("Connection"))
        connection.addArrangedSubview(row("Pi hostname", hostnameField))
        connection.addArrangedSubview(row("Network interface", networkPopup))
        connection.addArrangedSubview(row("Clock device", clockPopup))

        workgroupPopup.translatesAutoresizingMaskIntoConstraints = false
        jitterPopup.translatesAutoresizingMaskIntoConstraints = false
        for mode in ConfigStore.workgroupModes { workgroupPopup.addItem(withTitle: mode.title) }
        for frames in ConfigStore.jitterChoices { jitterPopup.addItem(withTitle: String(frames)) }

        let scheduling = NSStackView()
        scheduling.orientation = .vertical
        scheduling.alignment = .leading
        scheduling.spacing = 8
        scheduling.translatesAutoresizingMaskIntoConstraints = false
        scheduling.addArrangedSubview(sectionTitle("Realtime scheduling"))
        scheduling.addArrangedSubview(row("Workgroup", workgroupPopup))
        scheduling.addArrangedSubview(row("Jitter frames", jitterPopup))
        scheduling.addArrangedSubview(note("""
            Which CoreAudio workgroup the daemon's realtime thread joins. \
            The thread can hold exactly one. "Backend" is jackd's clock \
            device, which is the deadline the JACK graph enforces.
            """))

        for control in [netLatencyPopup, netRingPopup, governorPopup, qdiscPopup, eeePopup, offloadPopup] {
            control.translatesAutoresizingMaskIntoConstraints = false
        }
        for field in [rtNapiField, rtIrqField, cpuNetField, cpuDspField, nicRingField] {
            field.translatesAutoresizingMaskIntoConstraints = false
        }
        cpuNetField.placeholderString = "Leave unchanged (e.g. 3)"
        cpuDspField.placeholderString = "Leave unchanged (e.g. 0-2)"
        nicRingField.placeholderString = "Leave unchanged (e.g. 128)"
        for frames in ConfigStore.netLatencyChoices { netLatencyPopup.addItem(withTitle: String(frames)) }
        for frames in ConfigStore.netRingChoices { netRingPopup.addItem(withTitle: String(frames)) }
        for value in ConfigStore.governorChoices { governorPopup.addItem(withTitle: ConfigStore.title(for: value)) }
        for value in ConfigStore.qdiscChoices { qdiscPopup.addItem(withTitle: ConfigStore.title(for: value)) }
        for value in ConfigStore.onOffChoices { eeePopup.addItem(withTitle: ConfigStore.title(for: value)) }
        for value in ConfigStore.onOffChoices { offloadPopup.addItem(withTitle: ConfigStore.title(for: value)) }

        let pi = NSStackView()
        pi.orientation = .vertical
        pi.alignment = .leading
        pi.spacing = 8
        pi.translatesAutoresizingMaskIntoConstraints = false
        pi.addArrangedSubview(sectionTitle("Pi tuning"))
        pi.addArrangedSubview(row("Net latency (-l)", netLatencyPopup))
        pi.addArrangedSubview(row("Net ring (-g)", netRingPopup))
        pi.addArrangedSubview(row("NAPI priority", rtNapiField))
        pi.addArrangedSubview(row("IRQ priority", rtIrqField))
        pi.addArrangedSubview(row("Network CPUs", cpuNetField))
        pi.addArrangedSubview(row("Plugin CPUs", cpuDspField))
        pi.addArrangedSubview(row("CPU governor", governorPopup))
        pi.addArrangedSubview(row("Queue discipline", qdiscPopup))
        pi.addArrangedSubview(row("Energy Eff. Ethernet", eeePopup))
        pi.addArrangedSubview(row("NIC ring size", nicRingField))
        pi.addArrangedSubview(row("Offloads (gro/lro)", offloadPopup))
        pi.addArrangedSubview(note("""
            These are written to the Pi on every start — the Pi keeps no \
            setting of its own. The loop pair is coupled: half the ring must \
            exceed latency × period, or the Pi overruns while the Mac still \
            looks healthy. Blank fields leave that setting alone.
            """))

        let audio = NSStackView()
        audio.orientation = .vertical
        audio.alignment = .leading
        audio.spacing = 6
        audio.translatesAutoresizingMaskIntoConstraints = false
        audio.addArrangedSubview(sectionTitle("Audio status"))
        audio.addArrangedSubview(timingField)
        audio.addArrangedSubview(errorField)

        let probe = button("Probe Pi Now", #selector(probeNow(_:)))
        let reset = button("Reset to Defaults", #selector(resetDefaults(_:)))
        let cancel = button("Cancel", #selector(cancel(_:)))
        let apply = button("Apply", #selector(apply(_:)))
        apply.keyEquivalent = "\r"
        cancel.keyEquivalent = "\\e"
        let actions = NSStackView(views: [probe, reset, NSView(), cancel, apply])
        actions.orientation = .horizontal
        actions.spacing = 8
        actions.translatesAutoresizingMaskIntoConstraints = false

        content.addSubview(connection)
        content.addSubview(scheduling)
        content.addSubview(pi)
        content.addSubview(audio)
        content.addSubview(actions)
        NSLayoutConstraint.activate([
            connection.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
            connection.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
            connection.topAnchor.constraint(equalTo: content.topAnchor, constant: 20),
            scheduling.leadingAnchor.constraint(equalTo: connection.leadingAnchor),
            scheduling.trailingAnchor.constraint(equalTo: connection.trailingAnchor),
            scheduling.topAnchor.constraint(equalTo: connection.bottomAnchor, constant: 22),
            pi.leadingAnchor.constraint(equalTo: connection.leadingAnchor),
            pi.trailingAnchor.constraint(equalTo: connection.trailingAnchor),
            pi.topAnchor.constraint(equalTo: scheduling.bottomAnchor, constant: 22),
            audio.leadingAnchor.constraint(equalTo: connection.leadingAnchor),
            audio.trailingAnchor.constraint(equalTo: connection.trailingAnchor),
            audio.topAnchor.constraint(equalTo: pi.bottomAnchor, constant: 22),
            actions.leadingAnchor.constraint(equalTo: connection.leadingAnchor),
            actions.trailingAnchor.constraint(equalTo: connection.trailingAnchor),
            actions.topAnchor.constraint(equalTo: audio.bottomAnchor, constant: 22),
            actions.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -18),
            hostnameField.widthAnchor.constraint(equalToConstant: 300),
            networkPopup.widthAnchor.constraint(equalToConstant: 300),
            clockPopup.widthAnchor.constraint(equalToConstant: 300),
            workgroupPopup.widthAnchor.constraint(equalToConstant: 300),
            jitterPopup.widthAnchor.constraint(equalToConstant: 300),
            netLatencyPopup.widthAnchor.constraint(equalToConstant: 300),
            netRingPopup.widthAnchor.constraint(equalToConstant: 300),
            rtNapiField.widthAnchor.constraint(equalToConstant: 300),
            rtIrqField.widthAnchor.constraint(equalToConstant: 300),
            cpuNetField.widthAnchor.constraint(equalToConstant: 300),
            cpuDspField.widthAnchor.constraint(equalToConstant: 300),
            governorPopup.widthAnchor.constraint(equalToConstant: 300),
            qdiscPopup.widthAnchor.constraint(equalToConstant: 300),
            eeePopup.widthAnchor.constraint(equalToConstant: 300),
            nicRingField.widthAnchor.constraint(equalToConstant: 300),
            offloadPopup.widthAnchor.constraint(equalToConstant: 300),
        ])
        window?.setContentSize(NSSize(width: 520, height: 1))
        window?.layoutIfNeeded()
        window?.setContentSize(content.fittingSize)
    }

    private func note(_ text: String) -> NSTextField {
        let field = NSTextField(wrappingLabelWithString: text)
        field.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        field.textColor = .secondaryLabelColor
        field.widthAnchor.constraint(equalToConstant: 440).isActive = true
        return field
    }

    private func sectionTitle(_ title: String) -> NSTextField {
        let field = NSTextField(labelWithString: title)
        field.font = .boldSystemFont(ofSize: NSFont.systemFontSize)
        return field
    }

    private func row(_ title: String, _ control: NSView) -> NSView {
        let label = NSTextField(labelWithString: title)
        label.alignment = .right
        label.widthAnchor.constraint(equalToConstant: 130).isActive = true
        let stack = NSStackView(views: [label, control])
        stack.orientation = .horizontal
        stack.spacing = 10
        return stack
    }

    private func button(_ title: String, _ action: Selector) -> NSButton {
        let b = NSButton(title: title, target: self, action: action)
        b.bezelStyle = .rounded
        return b
    }

    private func showStatus(_ text: String, error: Bool = false) {
        errorField.textColor = error ? .systemRed : .secondaryLabelColor
        errorField.stringValue = text
    }

    private func reload() {
        timingIsLive = false
        do {
            values = try ConfigStore.load()
            loadError = nil
        } catch {
            values = ConfigStore.defaults
            loadError = error
        }
        populate()
        updateTiming()
        if let loadError { showStatus("Configuration read error: \(loadError.localizedDescription)", error: true) }
    }

    private func populate() {
        hostnameField.stringValue = values["PiHostname"] as? String ?? Defaults.hostname
        networkPopup.removeAllItems()
        networkPopup.addItem(withTitle: "Automatic")
        for iface in ConfigStore.interfaces() { networkPopup.addItem(withTitle: iface) }
        let network = values["NetworkInterface"] as? String ?? ""
        networkPopup.selectItem(withTitle: network.isEmpty ? "Automatic" : network)
        if networkPopup.selectedItem == nil { networkPopup.selectItem(at: 0) }

        clockPopup.removeAllItems()
        clockPopup.addItem(withTitle: "Automatic")
        for device in ConfigStore.audioDevices() {
            clockPopup.addItem(withTitle: device.name)
            clockPopup.lastItem?.representedObject = device.uid
        }
        let clock = values["ClockDeviceUID"] as? String ?? ""
        if let item = clockPopup.itemArray.first(where: { $0.representedObject as? String == clock }) {
            clockPopup.select(item)
        } else { clockPopup.selectItem(at: 0) }

        let workgroup = values["Workgroup"] as? String ?? ConfigStore.defaultWorkgroup
        let mode = ConfigStore.workgroupModes.first { $0.key == workgroup } ?? ConfigStore.workgroupModes[0]
        workgroupPopup.selectItem(withTitle: mode.title)

        // Tolerate either <integer> or <string>: config.plist is hand-editable.
        let jitter = (values["JitterFrames"] as? Int)
            ?? (values["JitterFrames"] as? String).flatMap(Int.init)
            ?? 0
        jitterPopup.selectItem(withTitle: String(jitter))
        if jitterPopup.selectedItem == nil { jitterPopup.selectItem(at: 0) }

        select(netLatencyPopup, String(ConfigStore.int(values, "NetLatency", 4)))
        select(netRingPopup, String(ConfigStore.int(values, "NetRing", 1024)))
        rtNapiField.stringValue = String(ConfigStore.int(values, "NetRtNapi", 60))
        rtIrqField.stringValue = String(ConfigStore.int(values, "NetRtIrq", 50))
        cpuNetField.stringValue = ConfigStore.string(values, "NetCpuNet")
        cpuDspField.stringValue = ConfigStore.string(values, "NetCpuDsp")
        nicRingField.stringValue = ConfigStore.string(values, "NetNicRing")
        select(governorPopup, ConfigStore.title(for: ConfigStore.string(values, "NetGovernor")))
        select(qdiscPopup, ConfigStore.title(for: ConfigStore.string(values, "NetQdisc")))
        select(eeePopup, ConfigStore.title(for: ConfigStore.string(values, "NetEEE")))
        select(offloadPopup, ConfigStore.title(for: ConfigStore.string(values, "NetNicOffload")))
    }

    // Select by title, falling back to the first item rather than leaving the
    // popup showing nothing when config.plist holds a value we don't offer.
    private func select(_ popup: NSPopUpButton, _ title: String) {
        popup.selectItem(withTitle: title)
        if popup.selectedItem == nil { popup.selectItem(at: 0) }
    }

    private func updateTiming() {
        if let timing = ConfigStore.cachedTiming() {
            let date = Date(timeIntervalSince1970: timing.timestamp)
            let age = max(0, Int(Date().timeIntervalSince(date)))
            let source = timingIsLive ? "Live probe" : "Cached, stale"
            let ageText = timingIsLive ? "" : " (\(age)s old)"
            timingField.stringValue = "Pi timing: \(timing.rate) Hz / \(timing.period) frames — \(source)\(ageText)"
        } else {
            timingField.stringValue = "Pi timing: No value"
        }
    }

    @objc private func probeNow(_ sender: Any?) {
        let hostname = hostnameField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        guard ConfigStore.validHostname(hostname) else {
            showStatus("Enter a valid Pi hostname before probing.", error: true)
            return
        }
        showStatus("Probing Pi…")
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                _ = try ConfigStore.probe(hostname: hostname)
                DispatchQueue.main.async {
                    self.timingIsLive = true
                    self.showStatus("Pi probe succeeded.")
                    self.updateTiming()
                }
            } catch {
                DispatchQueue.main.async {
                    self.showStatus("Pi probe failed: \(error.localizedDescription)", error: true)
                }
            }
        }
    }

    @objc private func resetDefaults(_ sender: Any?) {
        values = ConfigStore.defaults
        loadError = nil
        populate()
        updateTiming()
        showStatus("")
    }

    @objc private func cancel(_ sender: Any?) { window?.close() }

    @objc private func apply(_ sender: Any?) {
        let hostname = hostnameField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        guard ConfigStore.validHostname(hostname) else {
            showStatus("Enter a valid Pi hostname.", error: true)
            return
        }
        let network = networkPopup.selectedItem?.title == "Automatic" ? "" : (networkPopup.selectedItem?.title ?? "")
        guard network.isEmpty || ConfigStore.validInterface(network) else {
            showStatus("The selected network interface is invalid.", error: true)
            return
        }
        let clock = clockPopup.selectedItem?.representedObject as? String ?? ""

        // Pi tuning. Validate before the restart prompt: a bad value here is
        // worth catching now, not after the audio has stopped.
        let netLatency = Int(netLatencyPopup.selectedItem?.title ?? "") ?? 4
        let netRing = Int(netRingPopup.selectedItem?.title ?? "") ?? 1024
        // The loop pair is coupled. The resampler drives ring occupancy toward
        // the midpoint, so cushion beyond it overruns — and it overruns on the
        // *pi*, while every Mac-side metric stays clean, which reads as "the
        // change did nothing" rather than as a new fault.
        let period = Int(ConfigStore.cachedTiming()?.period ?? "") ?? 64
        guard netRing / 2 > netLatency * period else {
            showStatus("Net ring \(netRing) is too small for latency \(netLatency) at period \(period): "
                     + "half the ring (\(netRing / 2)) must exceed \(netLatency * period) frames.", error: true)
            return
        }
        guard let rtNapi = ConfigStore.priority(rtNapiField.stringValue),
              let rtIrq = ConfigStore.priority(rtIrqField.stringValue) else {
            showStatus("Realtime priorities must be between 1 and 99.", error: true)
            return
        }
        let cpuNet = cpuNetField.stringValue.trimmingCharacters(in: .whitespaces)
        let cpuDsp = cpuDspField.stringValue.trimmingCharacters(in: .whitespaces)
        guard ConfigStore.validCPUList(cpuNet), ConfigStore.validCPUList(cpuDsp) else {
            showStatus("CPU lists look like \"3\", \"0-2\" or \"0,2\" — or blank to leave alone.", error: true)
            return
        }
        let nicRing = nicRingField.stringValue.trimmingCharacters(in: .whitespaces)
        guard nicRing.isEmpty || (Int(nicRing).map { $0 > 0 && $0 <= 16384 } ?? false) else {
            showStatus("NIC ring size must be a positive number, or blank to leave it alone.", error: true)
            return
        }

        let alert = NSAlert()
        alert.messageText = "Restart JackBridge?"
        alert.informativeText = "Applying these settings restarts the existing JackBridge services. Audio will briefly stop."
        alert.addButton(withTitle: "Apply")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }

        var next = values
        next["PiHostname"] = hostname
        next["NetworkInterface"] = network
        next["ClockDeviceUID"] = clock
        next["Workgroup"] = ConfigStore.workgroupModes
            .first { $0.title == workgroupPopup.selectedItem?.title }?.key ?? ConfigStore.defaultWorkgroup
        next["JitterFrames"] = Int(jitterPopup.selectedItem?.title ?? "0") ?? 0
        next["NetLatency"] = netLatency
        next["NetRing"] = netRing
        next["NetRtNapi"] = rtNapi
        next["NetRtIrq"] = rtIrq
        next["NetCpuNet"] = cpuNet
        next["NetCpuDsp"] = cpuDsp
        next["NetNicRing"] = nicRing
        next["NetGovernor"] = ConfigStore.value(forTitle: governorPopup.selectedItem?.title)
        next["NetQdisc"] = ConfigStore.value(forTitle: qdiscPopup.selectedItem?.title)
        next["NetEEE"] = ConfigStore.value(forTitle: eeePopup.selectedItem?.title)
        next["NetNicOffload"] = ConfigStore.value(forTitle: offloadPopup.selectedItem?.title)
        do {
            try ConfigStore.write(next)
            values = next
            loadError = nil
            showStatus("Applied. launchd is restarting JackBridge.")
        } catch {
            showStatus("Configuration write error: \(error.localizedDescription)", error: true)
        }
    }
}

private struct AudioDevice {
    let uid: String
    let name: String
}

private enum ConfigStore {
    static let defaults: [String: Any] = [
        "PiHostname": "pistomp.local",
        "ClockDeviceUID": "",
        "NetworkInterface": "",
        // CoreAudio display-name base. The daemon renders the final name as
        // "<DeviceName> (<PiHostname>)"; override this to rename the product
        // half (e.g. a co-branded build) without touching code.
        "DeviceName": "pi-Stomp",
        "Workgroup": defaultWorkgroup,
        "JitterFrames": 0,
        // Pi tuning. Defaults are the pi's present behaviour, so adopting them
        // changes no timing; "" means "leave that setting alone on the pi",
        // which is not the same as a default. Mirrors PI_CONFIG_KEYS in
        // jackbridge/tools/jackbridge-ctl and the JACKBRIDGE_* table in
        // jackbridge/pi/bin/jackbridge-napi-rt.
        "NetLatency": 4,
        "NetRing": 1024,
        "NetRtNapi": 60,
        "NetRtIrq": 50,
        "NetCpuNet": "",
        "NetCpuDsp": "",
        "NetGovernor": "",
        "NetQdisc": "",
        "NetEEE": "",
        "NetNicRing": "",
        "NetNicOffload": "",
    ]

    static let netLatencyChoices = [2, 3, 4, 5, 6, 8]
    static let netRingChoices = [512, 1024, 2048, 4096]
    static let governorChoices = ["", "performance", "schedutil", "ondemand", "powersave"]
    static let qdiscChoices = ["", "pfifo_fast", "fq_codel", "noqueue"]
    static let onOffChoices = ["", "off", "on"]
    static let leaveAlone = "Leave unchanged"

    static func title(for value: String) -> String { value.isEmpty ? leaveAlone : value }
    static func value(forTitle title: String?) -> String {
        guard let title, title != leaveAlone else { return "" }
        return title
    }

    // config.plist is hand-editable, so tolerate either <integer> or <string>.
    static func int(_ values: [String: Any], _ key: String, _ fallback: Int) -> Int {
        (values[key] as? Int) ?? (values[key] as? String).flatMap(Int.init) ?? fallback
    }

    static func string(_ values: [String: Any], _ key: String) -> String {
        (values[key] as? String) ?? (values[key] as? Int).map(String.init) ?? ""
    }

    // SCHED_FIFO is 1..99.
    static func priority(_ text: String) -> Int? {
        guard let value = Int(text.trimmingCharacters(in: .whitespaces)),
              (1...99).contains(value) else { return nil }
        return value
    }

    // A CPU list as taskset and smp_affinity_list both spell it. Blank is
    // valid and means "leave the placement alone".
    static func validCPUList(_ value: String) -> Bool {
        if value.isEmpty { return true }
        guard value.count <= 32 else { return false }
        return value.allSatisfy { $0.isNumber || $0 == "," || $0 == "-" }
    }

    // Mirrors WorkgroupMode in jackbridge/daemon/JackBridge.cpp.
    static let workgroupModes: [(key: String, title: String)] = [
        ("backend", "Backend device (jackd clock)"),
        ("hal", "JackBridge HAL device"),
        ("none", "None (plain realtime)"),
    ]
    static let defaultWorkgroup = "backend"
    static let jitterChoices = [0, 64, 128, 256, 512, 1024]

    private static let forbidden = ["SampleRate", "PeriodFrames", "JackPrefix",
                                    "RealtimePriority", "NetJack", "AutoConnect", "Logging"]
    static let path = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/JackBridge/config.plist").path
    static let directory = (path as NSString).deletingLastPathComponent

    static func load() throws -> [String: Any] {
        if !FileManager.default.fileExists(atPath: path) {
            try seed()
        }
        guard let data = FileManager.default.contents(atPath: path) else { throw CocoaError(.fileReadNoSuchFile) }
        guard let plist = try PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any]
        else { throw CocoaError(.fileReadCorruptFile) }
        return plist
    }

    static func seed() throws {
        var source = defaults
        let shipped = "/Library/Application Support/JackBridge/config.plist.default"
        if let data = FileManager.default.contents(atPath: shipped),
           let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any] {
            source = plist
        }
        try write(source)
    }

    static func write(_ input: [String: Any]) throws {
        var output = input
        for key in forbidden { output.removeValue(forKey: key) }
        for (key, value) in defaults where output[key] == nil { output[key] = value }
        let data = try PropertyListSerialization.data(fromPropertyList: output, format: .xml, options: 0)
        try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true,
                                                  attributes: [.posixPermissions: 0o700])
        try? FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: directory)
        let tmp = "\(path).\(UUID().uuidString)"
        guard FileManager.default.createFile(atPath: tmp, contents: data,
                                              attributes: [.posixPermissions: 0o600]) else {
            throw CocoaError(.fileWriteUnknown)
        }
        do {
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: tmp)
            if FileManager.default.fileExists(atPath: path) {
                _ = try FileManager.default.replaceItemAt(URL(fileURLWithPath: path),
                                                          withItemAt: URL(fileURLWithPath: tmp),
                                                          backupItemName: nil,
                                                          options: .usingNewMetadataOnly)
            } else {
                try FileManager.default.moveItem(atPath: tmp, toPath: path)
            }
        } catch {
            try? FileManager.default.removeItem(atPath: tmp)
            throw error
        }
    }

    static func validHostname(_ value: String) -> Bool {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: ".-_:%"))
        return !value.isEmpty && value.count <= 253 && value.rangeOfCharacter(from: allowed.inverted) == nil
    }

    static func validInterface(_ value: String) -> Bool {
        guard !value.isEmpty, value.count <= 32 else { return false }
        return value.rangeOfCharacter(from: CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-").inverted) == nil
    }

    static func interfaces() -> [String] {
        let r = ProcessRunner.run("/sbin/ifconfig", args: ["-l"], timeout: 2)
        return r.stdout.split(whereSeparator: { $0 == " " || $0 == "\n" }).map(String.init)
            .filter(validInterface).sorted()
    }

    static func audioDevices() -> [AudioDevice] {
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                                  mScope: kAudioObjectPropertyScopeGlobal,
                                                  mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size) == noErr else { return [] }
        var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &ids) == noErr else { return [] }
        return ids.compactMap { id in
            guard let name = audioString(id, selector: kAudioObjectPropertyName),
                  let uid = audioString(id, selector: kAudioDevicePropertyDeviceUID),
                  !uid.contains("JackBridge"), hasOutput(id) else { return nil }
            return AudioDevice(uid: uid, name: name)
        }.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    private static func audioString(_ id: AudioDeviceID, selector: AudioObjectPropertySelector) -> String? {
        var address = AudioObjectPropertyAddress(mSelector: selector,
                                                  mScope: kAudioObjectPropertyScopeGlobal,
                                                  mElement: kAudioObjectPropertyElementMain)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &address, 0, nil, &size, &value) == noErr,
              let value else { return nil }
        return value.takeRetainedValue() as String
    }

    private static func hasOutput(_ id: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreamConfiguration,
                                                  mScope: kAudioObjectPropertyScopeOutput,
                                                  mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &address, 0, nil, &size) == noErr, size > 0 else { return false }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { raw.deallocate() }
        let list = raw.bindMemory(to: AudioBufferList.self, capacity: 1)
        guard AudioObjectGetPropertyData(id, &address, 0, nil, &size, list) == noErr else { return false }
        return UnsafeMutableAudioBufferListPointer(list).contains { $0.mNumberChannels > 0 }
    }

    static func probe(hostname: String) throws -> Timing {
        let ssh = ProcessInfo.processInfo.environment["JACKBRIDGE_SSH_PATH"] ?? "/usr/bin/ssh"
        let result = ProcessRunner.run(ssh, args: [
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=5",
            "-o", "ConnectionAttempts=1",
            "pistomp@\(hostname)",
            "cat", "/proc/asound/card0/pcm0p/sub0/hw_params",
        ], env: JackTools.environment, timeout: 8)
        guard result.status == 0, !result.timedOut else {
            let detail = result.timedOut ? "timed out" : "SSH unavailable"
            throw NSError(domain: "JackBridgeProbe", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Pi probe \(detail)"])
        }
        var rate: String?
        var period: String?
        for line in result.stdout.split(whereSeparator: \.isNewline) {
            let fields = line.split(whereSeparator: { $0 == " " || $0 == "\t" })
            if fields.first == "closed" { throw NSError(domain: "JackBridgeProbe", code: 2, userInfo: [NSLocalizedDescriptionKey: "Pi audio device is closed"]) }
            if fields.first == "rate:", fields.count > 1 { rate = String(fields[1]) }
            if fields.first == "period_size:", fields.count > 1 { period = String(fields[1]) }
        }
        guard let rate, let period, validRate(rate), validUInt32(period) else {
            throw NSError(domain: "JackBridgeProbe", code: 3, userInfo: [NSLocalizedDescriptionKey: "Pi timing data is missing or unsupported"])
        }
        let timing = Timing(rate: rate, period: period, timestamp: Date().timeIntervalSince1970)
        try saveTiming(timing)
        return timing
    }

    private static func validRate(_ value: String) -> Bool {
        value == "44100" || value == "48000" || value == "96000"
    }

    private static func validUInt32(_ value: String) -> Bool {
        guard let number = UInt64(value), number > 0, number <= UInt64(UInt32.max) else { return false }
        return !value.isEmpty && value.allSatisfy(\.isNumber)
    }

    private static func saveTiming(_ timing: Timing) throws {
        let text = "rate=\(timing.rate)\nperiod=\(timing.period)\nprobed_at=\(Int(timing.timestamp))\n"
        try atomicWrite(Data(text.utf8), to: directory + "/runtime-state")
    }

    private static func atomicWrite(_ data: Data, to destination: String) throws {
        try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true,
                                                  attributes: [.posixPermissions: 0o700])
        try? FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: directory)
        let temp = "\(destination).\(UUID().uuidString)"
        guard FileManager.default.createFile(atPath: temp, contents: data,
                                             attributes: [.posixPermissions: 0o600]) else {
            throw CocoaError(.fileWriteUnknown)
        }
        do {
            if FileManager.default.fileExists(atPath: destination) {
                _ = try FileManager.default.replaceItemAt(URL(fileURLWithPath: destination),
                                                          withItemAt: URL(fileURLWithPath: temp),
                                                          backupItemName: nil,
                                                          options: .usingNewMetadataOnly)
            } else {
                try FileManager.default.moveItem(atPath: temp, toPath: destination)
            }
        } catch {
            try? FileManager.default.removeItem(atPath: temp)
            throw error
        }
    }

    struct Timing { let rate: String; let period: String; let timestamp: TimeInterval }
    static func cachedTiming() -> Timing? {
        let state = directory + "/runtime-state"
        guard let text = try? String(contentsOfFile: state, encoding: .utf8) else { return nil }
        var fields: [String: String] = [:]
        for line in text.split(separator: "\n") {
            let parts = line.split(separator: "=", maxSplits: 1).map(String.init)
            if parts.count == 2 { fields[parts[0]] = parts[1] }
        }
        guard let rate = fields["rate"], let period = fields["period"],
              let stamp = fields["probed_at"], let timestamp = TimeInterval(stamp) else { return nil }
        return Timing(rate: rate, period: period, timestamp: timestamp)
    }
}
