#include "terminal_screen.h"

#include "console_input.h"
#include "terminal_platform.h"

#ifndef _WIN32

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cwchar>
#include <stdexcept>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace protected_chat::terminal_detail {

int platformCodePointWidth(std::uint32_t codePoint) noexcept {
    if (codePoint <= static_cast<std::uint32_t>(WCHAR_MAX) && MB_CUR_MAX > 1)
        return ::wcwidth(static_cast<wchar_t>(codePoint));
    return -1;
}

} // namespace protected_chat::terminal_detail

TerminalScreen::TerminalScreen(ConsoleInput& newInput)
    : input(newInput), terminalChanged(false), screenEntered(false),
      lastRows(24), lastColumns(80), lastSizeQueryFailed(false),
      originalSettingsStorage{} {
    static_assert(sizeof(originalSettingsStorage) >= sizeof(struct termios),
                  "termios storage is too small");
    const char* term = std::getenv("TERM");
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO) || term == nullptr ||
        std::string(term).empty() || std::string(term) == "dumb") {
        throw std::runtime_error("Full-screen terminal requires an ANSI TTY");
    }
    auto* original = reinterpret_cast<struct termios*>(originalSettingsStorage);
    if (::tcgetattr(STDIN_FILENO, original) != 0)
        throw std::runtime_error("Could not read terminal settings");
    struct termios raw = *original;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | IEXTEN | ISIG));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | INLCR | IGNCR));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        throw std::runtime_error("Could not enter terminal input mode");
    terminalChanged = true;
    try {
        input.enableResizeNotifications();
        screenEntered = true;
        writeOutput("\x1b[?1049h\x1b[0m\x1b[2J\x1b[H\x1b[?2004h");
    } catch (...) {
        restore();
        throw;
    }
}

TerminalScreen::~TerminalScreen() noexcept { restore(); }

void TerminalScreen::restore() noexcept {
    if (screenEntered) {
        try { writeOutput("\x1b[?2004l\x1b[0m\x1b[?25h\x1b[?1049l"); }
        catch (...) {}
        screenEntered = false;
    }
    input.disableResizeNotifications();
    if (terminalChanged) {
        const auto* original =
            reinterpret_cast<const struct termios*>(originalSettingsStorage);
        ::tcflush(STDIN_FILENO, TCIFLUSH);
        ::tcsetattr(STDIN_FILENO, TCSANOW, original);
        terminalChanged = false;
    }
}

TerminalSize TerminalScreen::size() const {
    struct winsize current{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &current) != 0 ||
        current.ws_row == 0 || current.ws_col == 0) {
        lastSizeQueryFailed = true;
        return TerminalSize(lastRows, lastColumns);
    }
    lastRows = std::min<std::size_t>(current.ws_row, 120);
    lastColumns = std::min<std::size_t>(current.ws_col, 240);
    lastSizeQueryFailed = false;
    return TerminalSize(lastRows, lastColumns);
}

void TerminalScreen::writeOutput(const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(
            STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) offset += static_cast<std::size_t>(count);
        else if (count == -1 && errno == EINTR) continue;
        else throw std::runtime_error("Terminal write failed");
    }
}

#endif
