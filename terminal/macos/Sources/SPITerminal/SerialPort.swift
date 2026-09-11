import Foundation

enum SerialError: LocalizedError {
    case openFailed(String)
    case configureFailed(String)

    var errorDescription: String? {
        switch self {
        case .openFailed(let s): return "Cannot open \(s)"
        case .configureFailed(let s): return "Cannot configure \(s)"
        }
    }
}

/// Minimal POSIX (termios) serial port on /dev/cu.* — 115200 8N1 raw.
final class SerialPort {
    let path: String
    private(set) var fd: Int32 = -1

    init(path: String) {
        self.path = path
    }

    var isOpen: Bool { fd >= 0 }

    static func availablePorts() -> [String] {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(atPath: "/dev") else {
            return []
        }
        return entries.filter { $0.hasPrefix("cu.") }.sorted()
    }

    /// Opens and configures the port (115200, 8N1, raw, non-blocking).
    func open(baud: speed_t = speed_t(B115200)) throws {
        guard !isOpen else { return }

        let newFd = Darwin.open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard newFd >= 0 else {
            throw SerialError.openFailed("\(path): \(String(cString: strerror(errno)))")
        }

        var tty = termios()
        guard tcgetattr(newFd, &tty) == 0 else {
            Darwin.close(newFd)
            throw SerialError.configureFailed("\(path): tcgetattr failed")
        }
        cfmakeraw(&tty)
        // tcflag_t imports as UInt on macOS; keep the flag maths in UInt.
        tty.c_cflag |= UInt(CREAD | CLOCAL)
        tty.c_cflag &= ~UInt(CSTOPB)   // 1 stop bit
        tty.c_cflag &= ~UInt(PARENB)   // no parity
        tty.c_cflag = (tty.c_cflag & ~UInt(CSIZE)) | UInt(CS8)
        tty.c_cflag &= ~UInt(CRTSCTS)  // no hardware flow control
        cfsetispeed(&tty, baud)
        cfsetospeed(&tty, baud)
        tty.c_cc.16 = 1 // VMIN
        tty.c_cc.17 = 0 // VTIME
        guard tcsetattr(newFd, TCSANOW, &tty) == 0 else {
            Darwin.close(newFd)
            throw SerialError.configureFailed("\(path): tcsetattr failed")
        }

        fd = newFd
    }

    /// Non-blocking read; returns bytes read, 0 if none available, -1 on error.
    func read(into buffer: UnsafeMutablePointer<UInt8>, capacity: Int) -> Int {
        guard isOpen else { return -1 }
        let n = Darwin.read(fd, buffer, capacity)
        if n < 0 && (errno == EAGAIN || errno == EINTR) {
            return 0
        }
        return n
    }

    func write(_ data: Data) {
        guard isOpen else { return }
        data.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return }
            _ = Darwin.write(fd, base, data.count)
        }
    }

    func close() {
        if fd >= 0 {
            Darwin.close(fd)
            fd = -1
        }
    }

    deinit {
        close()
    }
}
