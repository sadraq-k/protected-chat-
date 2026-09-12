#ifndef PROTECTED_CHAT_TERMINAL_SCREEN_H
#define PROTECTED_CHAT_TERMINAL_SCREEN_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class ConsoleInput;

class TerminalSize {
public:
    TerminalSize(std::size_t rows, std::size_t columns);
    std::size_t rows() const noexcept;
    std::size_t columns() const noexcept;
private:
    std::size_t terminalRows;
    std::size_t terminalColumns;
};

class ScreenFrame {
public:
    ScreenFrame(
        std::vector<std::string> rows,
        std::size_t cursorRow,
        std::size_t cursorColumn,
        bool cursorVisible = true);
    const std::vector<std::string>& rows() const noexcept;
    std::size_t cursorRow() const noexcept;
    std::size_t cursorColumn() const noexcept;
    bool cursorVisible() const noexcept;
private:
    std::vector<std::string> screenRows;
    std::size_t inputCursorRow;
    std::size_t inputCursorColumn;
    bool showCursor;
};

namespace protected_chat::terminal_detail {
bool validUtf8(const std::string& text) noexcept;
std::string safeText(const std::string& text);
std::size_t displayWidth(const std::string& safeText) noexcept;
std::string clipToWidth(const std::string& safeText, std::size_t width);
std::vector<std::string> wrapToWidth(
    const std::string& safeText, std::size_t width);
}

class TerminalScreen {
public:
    explicit TerminalScreen(ConsoleInput& input);
    ~TerminalScreen() noexcept;
    TerminalScreen(const TerminalScreen&) = delete;
    TerminalScreen& operator=(const TerminalScreen&) = delete;
    TerminalScreen(TerminalScreen&&) = delete;
    TerminalScreen& operator=(TerminalScreen&&) = delete;

    TerminalSize size() const;
    bool sizeQueryFailed() const noexcept;
    void present(const ScreenFrame& frame);

private:
    void restore() noexcept;
    void writeOutput(const std::string& bytes);
#ifdef _WIN32
    class WindowsState;
#endif
    ConsoleInput& input;
    bool terminalChanged;
    bool screenEntered;
    mutable std::size_t lastRows;
    mutable std::size_t lastColumns;
    mutable bool lastSizeQueryFailed;
#ifndef _WIN32
    alignas(void*) unsigned char originalSettingsStorage[128];
#else
    std::unique_ptr<WindowsState> windowsState;
#endif
};

#endif
