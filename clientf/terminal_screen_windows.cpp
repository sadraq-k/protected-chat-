#include "terminal_screen.h"

#include "console_input.h"
#include "terminal_platform.h"

#ifdef _WIN32

#include <windows.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

bool inRange(std::uint32_t value, std::uint32_t first, std::uint32_t last) {
    return value >= first && value <= last;
}

} // namespace

namespace protected_chat::terminal_detail {

std::wstring utf8ToUtf16(const std::string& text) {
    if (text.empty()) return std::wstring();
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("UTF-8 input is too large");
    const int sourceLength = static_cast<int>(text.size());
    const int required = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), sourceLength, nullptr, 0);
    if (required <= 0) throw std::runtime_error("Invalid UTF-8 input");
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), sourceLength,
            result.data(), required) != required) {
        throw std::runtime_error("UTF-8 conversion failed");
    }
    return result;
}

std::string utf16ToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("UTF-16 input is too large");
    const int sourceLength = static_cast<int>(text.size());
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), sourceLength,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("Invalid UTF-16 input");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), sourceLength,
            result.data(), required, nullptr, nullptr) != required) {
        throw std::runtime_error("UTF-16 conversion failed");
    }
    return result;
}

int platformCodePointWidth(std::uint32_t value) noexcept {
    if (value > 0xffffu || inRange(value, 0xd800u, 0xdfffu)) return -1;
    if (value == 0x00adu || value == 0x034fu ||
        inRange(value, 0x0600u, 0x0605u) || value == 0x061cu ||
        value == 0x06ddu || value == 0x070fu ||
        inRange(value, 0x0890u, 0x0891u) || value == 0x08e2u ||
        inRange(value, 0x115fu, 0x1160u) ||
        inRange(value, 0x17b4u, 0x17b5u) ||
        inRange(value, 0x180bu, 0x180fu) ||
        inRange(value, 0x200bu, 0x200fu) ||
        inRange(value, 0x2028u, 0x202eu) ||
        inRange(value, 0x2060u, 0x206fu) || value == 0x3164u ||
        inRange(value, 0xfe00u, 0xfe0fu) || value == 0xfeffu ||
        value == 0xffa0u || inRange(value, 0xfff9u, 0xffffu)) return -1;

    const wchar_t character = static_cast<wchar_t>(value);
    WORD type1 = 0;
    WORD type3 = 0;
    if (!::GetStringTypeW(CT_CTYPE1, &character, 1, &type1) ||
        !::GetStringTypeW(CT_CTYPE3, &character, 1, &type3) ||
        type1 == 0 || (type1 & C1_CNTRL) != 0 ||
        (type3 & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) != 0) {
        return -1;
    }

    const bool symbolException = value == 0x2329u || value == 0x232au;
    if ((!symbolException && inRange(value, 0x2300u, 0x27ffu)) ||
        inRange(value, 0x2b00u, 0x2bffu) ||
        inRange(value, 0xe000u, 0xf8ffu)) return -1;

    if (inRange(value, 0x1100u, 0x115eu) || symbolException ||
        inRange(value, 0x2e80u, 0xa4cfu) ||
        inRange(value, 0xac00u, 0xd7a3u) ||
        inRange(value, 0xf900u, 0xfaffu) ||
        inRange(value, 0xfe10u, 0xfe19u) ||
        inRange(value, 0xfe30u, 0xfe6fu) ||
        inRange(value, 0xff01u, 0xff60u) ||
        inRange(value, 0xffe0u, 0xffe6u)) return 2;
    return 1;
}

} // namespace protected_chat::terminal_detail

class TerminalScreen::WindowsState {
public:
    HANDLE outputHandle = INVALID_HANDLE_VALUE;
    DWORD originalOutputMode = 0;
    CONSOLE_CURSOR_INFO originalCursor{};
    COORD originalCursorPosition{};
    WORD originalAttributes = 0;
    bool outputModeCaptured = false;
    bool cursorCaptured = false;
    bool screenInfoCaptured = false;
};

TerminalScreen::TerminalScreen(ConsoleInput& newInput)
    : input(newInput), terminalChanged(false), screenEntered(false),
      lastRows(24), lastColumns(80), lastSizeQueryFailed(false),
      windowsState(std::make_unique<WindowsState>()) {
    WindowsState& state = *windowsState;
    state.outputHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (state.outputHandle == nullptr ||
        state.outputHandle == INVALID_HANDLE_VALUE ||
        !::GetConsoleMode(state.outputHandle, &state.originalOutputMode)) {
        throw std::runtime_error("Full-screen terminal requires a Windows console");
    }
    state.outputModeCaptured = true;

    CONSOLE_SCREEN_BUFFER_INFO screenInfo{};
    if (!::GetConsoleScreenBufferInfo(state.outputHandle, &screenInfo) ||
        !::GetConsoleCursorInfo(state.outputHandle, &state.originalCursor)) {
        throw std::runtime_error("Could not read Windows console state");
    }
    state.originalCursorPosition = screenInfo.dwCursorPosition;
    state.originalAttributes = screenInfo.wAttributes;
    state.screenInfoCaptured = true;
    state.cursorCaptured = true;

    try {
        const DWORD mode = state.originalOutputMode |
            ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (!::SetConsoleMode(state.outputHandle, mode))
            throw std::runtime_error("Could not configure Windows terminal output");
        terminalChanged = true;
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
    if (!windowsState) return;
    WindowsState& state = *windowsState;
    if (screenEntered) {
        try { writeOutput("\x1b[?2004l"); } catch (...) {}
        try { writeOutput("\x1b[0m"); } catch (...) {}
        try { writeOutput("\x1b[?25h"); } catch (...) {}
        try { writeOutput("\x1b[?1049l"); } catch (...) {}
        screenEntered = false;
    }
    input.disableResizeNotifications();
    if (terminalChanged && state.outputModeCaptured)
        ::SetConsoleMode(state.outputHandle, state.originalOutputMode);
    if (state.screenInfoCaptured)
        ::SetConsoleTextAttribute(state.outputHandle, state.originalAttributes);
    if (state.cursorCaptured)
        ::SetConsoleCursorInfo(state.outputHandle, &state.originalCursor);
    if (state.screenInfoCaptured) {
        CONSOLE_SCREEN_BUFFER_INFO current{};
        COORD position = state.originalCursorPosition;
        if (::GetConsoleScreenBufferInfo(state.outputHandle, &current) &&
            current.dwSize.X > 0 && current.dwSize.Y > 0) {
            position.X = std::max<SHORT>(
                0, std::min<SHORT>(position.X, current.dwSize.X - 1));
            position.Y = std::max<SHORT>(
                0, std::min<SHORT>(position.Y, current.dwSize.Y - 1));
            ::SetConsoleCursorPosition(state.outputHandle, position);
        }
    }
    terminalChanged = false;
}

TerminalSize TerminalScreen::size() const {
    if (!windowsState) {
        lastSizeQueryFailed = true;
        return TerminalSize(lastRows, lastColumns);
    }
    CONSOLE_SCREEN_BUFFER_INFO current{};
    if (!::GetConsoleScreenBufferInfo(windowsState->outputHandle, &current)) {
        lastSizeQueryFailed = true;
        return TerminalSize(lastRows, lastColumns);
    }
    const LONG rows = static_cast<LONG>(current.srWindow.Bottom) -
        static_cast<LONG>(current.srWindow.Top) + 1;
    const LONG columns = static_cast<LONG>(current.srWindow.Right) -
        static_cast<LONG>(current.srWindow.Left) + 1;
    if (rows <= 0 || columns <= 0) {
        lastSizeQueryFailed = true;
        return TerminalSize(lastRows, lastColumns);
    }
    lastRows = std::min<std::size_t>(static_cast<std::size_t>(rows), 120);
    lastColumns = std::min<std::size_t>(static_cast<std::size_t>(columns), 240);
    lastSizeQueryFailed = false;
    return TerminalSize(lastRows, lastColumns);
}

void TerminalScreen::writeOutput(const std::string& bytes) {
    if (!windowsState) throw std::runtime_error("Windows terminal output is unavailable");
    const std::wstring text = protected_chat::terminal_detail::utf8ToUtf16(bytes);
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::size_t count = std::min<std::size_t>(16'384, text.size() - offset);
        if (offset + count < text.size() && count > 0 &&
            text[offset + count - 1] >= 0xd800 &&
            text[offset + count - 1] <= 0xdbff &&
            text[offset + count] >= 0xdc00 && text[offset + count] <= 0xdfff) {
            --count;
        }
        DWORD written = 0;
        if (count == 0 || !::WriteConsoleW(
                windowsState->outputHandle, text.data() + offset,
                static_cast<DWORD>(count), &written, nullptr) || written == 0) {
            throw std::runtime_error("Windows terminal output failed");
        }
        offset += static_cast<std::size_t>(written);
    }
}

#endif
