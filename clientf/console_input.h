#ifndef CONSOLE_INPUT_H
#define CONSOLE_INPUT_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

enum class ConsoleReadResult {
    Line, WorkAvailable, EndOfFile, Interrupted, TooLong, Unsupported, Failure
};

enum class ConsoleKey {
    Text, Paste, Enter, Escape, Backspace, Delete,
    Left, Right, Home, End, PageUp, PageDown, FollowLatest,
    Notifications, Exit, Redraw, ClearInput, RestoreFailed,
    WorkAvailable, Interrupted, EndOfFile, Unsupported, Failure,
    UnsupportedKey, InputTooLarge
};

class ConsoleEvent {
public:
    explicit ConsoleEvent(ConsoleKey key, std::string text = std::string());
    ConsoleKey key() const noexcept;
    const std::string& text() const noexcept;
private:
    ConsoleKey eventKey;
    std::string eventText;
};

class ConsoleInput {
public:
    static constexpr std::size_t MaxLineBytes = 1'048'576;
    static constexpr std::size_t MaxPasteBytes = 65'536;

    ConsoleInput();
    ~ConsoleInput() noexcept;
    ConsoleInput(const ConsoleInput&) = delete;
    ConsoleInput& operator=(const ConsoleInput&) = delete;
    ConsoleInput(ConsoleInput&&) = delete;
    ConsoleInput& operator=(ConsoleInput&&) = delete;

    ConsoleReadResult readLine(std::string& line);
    ConsoleEvent readEvent();
    void notifyWork() noexcept;
    void interrupt() noexcept;
    void enableResizeNotifications();
    void disableResizeNotifications() noexcept;

private:
#ifdef _WIN32
    class WindowsState;
#else
    ConsoleEvent readPosixEvent();
    bool fillEventInput(int timeoutMilliseconds, ConsoleKey& wakeEvent);
    void writeWakeByte(char byte) noexcept;
#endif
    std::string inputBuffer;
    std::string pasteBuffer;
    std::atomic<bool> interrupted;
    std::atomic<bool> workPending;
    bool endOfFileSeen;
    bool pasteActive;
    bool pasteTooLarge;
    bool preferBufferedInput;
    bool escapePending;
    std::chrono::steady_clock::time_point escapeDeadline;
#ifndef _WIN32
    int wakeReadDescriptor;
    int wakeWriteDescriptor;
    bool resizeNotificationsEnabled;
    bool previousResizeActionValid;
    alignas(void*) unsigned char previousResizeActionStorage[256];
#else
    std::unique_ptr<WindowsState> windowsState;
#endif
};

#endif
