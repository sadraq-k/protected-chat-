#include "terminal_screen.h"

#include "console_input.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

class DisplayUnit {
public:
    DisplayUnit(std::string bytes, std::size_t width)
        : bytes(std::move(bytes)), width(width) {}
    std::string bytes;
    std::size_t width;
};

bool continuation(unsigned char byte) {
    return (byte & 0xc0u) == 0x80u;
}

bool decodeCodePoint(
    const std::string& text,
    std::size_t offset,
    std::uint32_t& codePoint,
    std::size_t& length) {
    const unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80u) {
        codePoint = first;
        length = 1;
        return true;
    }
    if (first >= 0xc2u && first <= 0xdfu) length = 2;
    else if (first >= 0xe0u && first <= 0xefu) length = 3;
    else if (first >= 0xf0u && first <= 0xf4u) length = 4;
    else return false;
    if (offset + length > text.size()) return false;
    for (std::size_t index = 1; index < length; ++index)
        if (!continuation(static_cast<unsigned char>(text[offset + index])))
            return false;
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if ((first == 0xe0u && second < 0xa0u) ||
        (first == 0xedu && second >= 0xa0u) ||
        (first == 0xf0u && second < 0x90u) ||
        (first == 0xf4u && second >= 0x90u)) return false;
    if (length == 2)
        codePoint = ((first & 0x1fu) << 6) | (second & 0x3fu);
    else if (length == 3)
        codePoint = ((first & 0x0fu) << 12) | ((second & 0x3fu) << 6) |
            (static_cast<unsigned char>(text[offset + 2]) & 0x3fu);
    else
        codePoint = ((first & 0x07u) << 18) | ((second & 0x3fu) << 12) |
            ((static_cast<unsigned char>(text[offset + 2]) & 0x3fu) << 6) |
            (static_cast<unsigned char>(text[offset + 3]) & 0x3fu);
    return true;
}

std::string hexByte(unsigned char byte) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result = "\\x00";
    result[2] = digits[byte >> 4];
    result[3] = digits[byte & 0x0fu];
    return result;
}

std::string hexCodePoint(std::uint32_t value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string body;
    do {
        body.push_back(digits[value & 0x0fu]);
        value >>= 4;
    } while (value != 0);
    std::reverse(body.begin(), body.end());
    return "\\u{" + body + "}";
}

bool unsafeCodePoint(std::uint32_t value) {
    return value < 0x20u || (value >= 0x7fu && value <= 0x9fu) ||
        value == 0x202au || value == 0x202bu || value == 0x202du ||
        value == 0x202eu || value == 0x202cu || value == 0x2066u ||
        value == 0x2067u || value == 0x2068u || value == 0x2069u ||
        value == 0x200bu || value == 0x200cu || value == 0x200du ||
        value == 0x2060u || value == 0xfeffu;
}

std::vector<DisplayUnit> units(const std::string& text) {
    std::vector<DisplayUnit> result;
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::uint32_t codePoint = 0;
        std::size_t length = 0;
        if (!decodeCodePoint(text, offset, codePoint, length)) {
            result.emplace_back(
                hexByte(static_cast<unsigned char>(text[offset])), 4);
            ++offset;
            continue;
        }
        if (unsafeCodePoint(codePoint)) {
            const std::string escaped = codePoint <= 0xffu
                ? hexByte(static_cast<unsigned char>(codePoint))
                : hexCodePoint(codePoint);
            result.emplace_back(escaped, escaped.size());
            offset += length;
            continue;
        }
        if (codePoint < 0x7fu) {
            result.emplace_back(text.substr(offset, length), 1);
            offset += length;
            continue;
        }
        int width = -1;
        if (codePoint <= static_cast<std::uint32_t>(WCHAR_MAX) && MB_CUR_MAX > 1)
            width = ::wcwidth(static_cast<wchar_t>(codePoint));
        if (width <= 0) {
            const std::string escaped = hexCodePoint(codePoint);
            result.emplace_back(escaped, escaped.size());
        } else {
            result.emplace_back(text.substr(offset, length),
                                static_cast<std::size_t>(width));
        }
        offset += length;
    }
    return result;
}

void writeAll(const std::string& bytes) {
#ifndef _WIN32
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(
            STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) offset += static_cast<std::size_t>(count);
        else if (count == -1 && errno == EINTR) continue;
        else throw std::runtime_error("Terminal write failed");
    }
#else
    (void)bytes;
#endif
}

} // namespace

TerminalSize::TerminalSize(std::size_t rows, std::size_t columns)
    : terminalRows(rows), terminalColumns(columns) {}
std::size_t TerminalSize::rows() const noexcept { return terminalRows; }
std::size_t TerminalSize::columns() const noexcept { return terminalColumns; }

ScreenFrame::ScreenFrame(
    std::vector<std::string> rows, std::size_t cursorRow,
    std::size_t cursorColumn, bool cursorVisible)
    : screenRows(std::move(rows)), inputCursorRow(cursorRow),
      inputCursorColumn(cursorColumn), showCursor(cursorVisible) {}
const std::vector<std::string>& ScreenFrame::rows() const noexcept {
    return screenRows;
}
std::size_t ScreenFrame::cursorRow() const noexcept { return inputCursorRow; }
std::size_t ScreenFrame::cursorColumn() const noexcept { return inputCursorColumn; }
bool ScreenFrame::cursorVisible() const noexcept { return showCursor; }

namespace protected_chat::terminal_detail {

bool validUtf8(const std::string& text) noexcept {
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::uint32_t codePoint = 0;
        std::size_t length = 0;
        if (!decodeCodePoint(text, offset, codePoint, length)) return false;
        offset += length;
    }
    return true;
}

std::string safeText(const std::string& text) {
    std::string result;
    for (const auto& unit : units(text)) result += unit.bytes;
    return result;
}

std::size_t displayWidth(const std::string& text) noexcept {
    std::size_t width = 0;
    try {
        for (const auto& unit : units(text)) width += unit.width;
    } catch (...) {
        return std::numeric_limits<std::size_t>::max();
    }
    return width;
}

std::string clipToWidth(const std::string& text, std::size_t width) {
    std::string result;
    std::size_t used = 0;
    for (const auto& unit : units(text)) {
        if (unit.width > width - used) break;
        result += unit.bytes;
        used += unit.width;
    }
    return result;
}

std::vector<std::string> wrapToWidth(
    const std::string& text, std::size_t width) {
    std::vector<std::string> rows(1);
    if (width == 0) return rows;
    std::size_t used = 0;
    for (const auto& unit : units(text)) {
        if (used != 0 && unit.width > width - used) {
            rows.emplace_back();
            used = 0;
        }
        rows.back() += unit.bytes;
        used += unit.width;
    }
    return rows;
}

} // namespace protected_chat::terminal_detail

TerminalScreen::TerminalScreen(ConsoleInput& newInput)
    : input(newInput), terminalChanged(false), screenEntered(false),
      lastRows(24), lastColumns(80), lastSizeQueryFailed(false)
#ifndef _WIN32
      , originalSettingsStorage{}
#endif
{
#ifdef _WIN32
    throw std::runtime_error("Full-screen terminal UI is unsupported on Windows");
#else
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
        writeAll("\x1b[?1049h\x1b[0m\x1b[2J\x1b[H\x1b[?2004h");
    } catch (...) {
        restore();
        throw;
    }
#endif
}

TerminalScreen::~TerminalScreen() noexcept { restore(); }

void TerminalScreen::restore() noexcept {
#ifndef _WIN32
    if (screenEntered) {
        try { writeAll("\x1b[?2004l\x1b[0m\x1b[?25h\x1b[?1049l"); }
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
#endif
}

TerminalSize TerminalScreen::size() const {
#ifdef _WIN32
    return TerminalSize(24, 80);
#else
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
#endif
}

bool TerminalScreen::sizeQueryFailed() const noexcept {
    return lastSizeQueryFailed;
}

void TerminalScreen::present(const ScreenFrame& frame) {
    const TerminalSize current = size();
    const std::size_t rows = current.rows();
    const std::size_t width = current.columns() > 0 ? current.columns() - 1 : 0;
    std::string output = "\x1b[?25l";
    for (std::size_t row = 0; row < rows; ++row) {
        output += "\x1b[" + std::to_string(row + 1) + ";1H\x1b[2K";
        if (row < frame.rows().size())
            output += protected_chat::terminal_detail::clipToWidth(
                frame.rows()[row], width);
    }
    if (frame.cursorVisible()) {
        output += "\x1b[" + std::to_string(
            std::min(frame.cursorRow() + 1, rows)) + ";" +
            std::to_string(std::min(frame.cursorColumn() + 1,
                                    current.columns())) + "H\x1b[?25h";
    }
    writeAll(output);
}
