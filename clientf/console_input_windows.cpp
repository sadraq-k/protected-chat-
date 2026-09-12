#include "console_input.h"

#include "terminal_platform.h"
#include "terminal_screen.h"

#ifdef _WIN32

#include <windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

SRWLOCK registrationLock = SRWLOCK_INIT;
HANDLE registeredStopEvent = nullptr;
HANDLE registeredCleanupEvent = nullptr;
HANDLE registeredHandlersIdleEvent = nullptr;
LONG activeHandlers = 0;
bool registrationActive = false;

BOOL WINAPI consoleControlHandler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT &&
        event != CTRL_CLOSE_EVENT) return FALSE;

    HANDLE stopEvent = nullptr;
    HANDLE cleanupEvent = nullptr;
    HANDLE handlersIdleEvent = nullptr;
    ::AcquireSRWLockExclusive(&registrationLock);
    if (registrationActive) {
        stopEvent = registeredStopEvent;
        cleanupEvent = registeredCleanupEvent;
        handlersIdleEvent = registeredHandlersIdleEvent;
        ++activeHandlers;
        ::ResetEvent(handlersIdleEvent);
    }
    ::ReleaseSRWLockExclusive(&registrationLock);
    if (stopEvent == nullptr) return FALSE;

    ::SetEvent(stopEvent);
    if (event == CTRL_CLOSE_EVENT) ::WaitForSingleObject(cleanupEvent, 4'000);

    ::AcquireSRWLockExclusive(&registrationLock);
    --activeHandlers;
    if (activeHandlers == 0) ::SetEvent(handlersIdleEvent);
    ::ReleaseSRWLockExclusive(&registrationLock);
    return TRUE;
}

bool highSurrogate(wchar_t value) {
    return value >= 0xd800 && value <= 0xdbff;
}

bool lowSurrogate(wchar_t value) {
    return value >= 0xdc00 && value <= 0xdfff;
}

bool pasteContainsControl(const std::string& text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        if (byte < 0x20u || byte == 0x7fu) return true;
        if (byte == 0xc2u && index + 1 < text.size()) {
            const unsigned char next = static_cast<unsigned char>(text[index + 1]);
            if (next >= 0x80u && next <= 0x9fu) return true;
        }
    }
    return false;
}

void removeLastUtf8Scalar(std::string& text) {
    if (text.empty()) return;
    std::size_t offset = text.size() - 1;
    while (offset > 0 &&
           (static_cast<unsigned char>(text[offset]) & 0xc0u) == 0x80u) {
        --offset;
    }
    text.erase(offset);
}

ConsoleKey vtKey(const std::wstring& sequence) {
    if (sequence == L"\x1b[D" || sequence == L"\x1bOD") return ConsoleKey::Left;
    if (sequence == L"\x1b[C" || sequence == L"\x1bOC") return ConsoleKey::Right;
    if (sequence == L"\x1b[H" || sequence == L"\x1b[1~" ||
        sequence == L"\x1bOH") return ConsoleKey::Home;
    if (sequence == L"\x1b[F" || sequence == L"\x1b[4~" ||
        sequence == L"\x1bOF") return ConsoleKey::End;
    if (sequence == L"\x1b[3~") return ConsoleKey::Delete;
    if (sequence == L"\x1b[5~") return ConsoleKey::PageUp;
    if (sequence == L"\x1b[6~") return ConsoleKey::PageDown;
    if (sequence == L"\x1b[1;5F") return ConsoleKey::FollowLatest;
    return ConsoleKey::UnsupportedKey;
}

} // namespace

class ConsoleInput::WindowsState {
public:
    WindowsState() {
        try {
            inputHandle = ::GetStdHandle(STD_INPUT_HANDLE);
            outputHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
            if (inputHandle == nullptr || inputHandle == INVALID_HANDLE_VALUE ||
                outputHandle == nullptr || outputHandle == INVALID_HANDLE_VALUE ||
                !::GetConsoleMode(inputHandle, &originalInputMode) ||
                !::GetConsoleMode(outputHandle, &originalOutputMode)) {
                throw std::runtime_error("Windows console input is unavailable");
            }
            originalOutputCodePage = ::GetConsoleOutputCP();
            if (originalOutputCodePage == 0)
                throw std::runtime_error("Could not read Windows console encoding");

            stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            workEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            cleanupCompleteEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            handlersIdleEvent = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
            if (stopEvent == nullptr || workEvent == nullptr ||
                cleanupCompleteEvent == nullptr || handlersIdleEvent == nullptr) {
                throw std::runtime_error("Could not initialize Windows console events");
            }

            outerInputMode = (originalInputMode | ENABLE_EXTENDED_FLAGS |
                ENABLE_VIRTUAL_TERMINAL_INPUT) &
                ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                  ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT);
            if (!::SetConsoleMode(inputHandle, outerInputMode))
                throw std::runtime_error("Could not configure Windows console input");
            inputModeChanged = true;
            DWORD actualMode = 0;
            if (!::GetConsoleMode(inputHandle, &actualMode) ||
                actualMode != outerInputMode) {
                throw std::runtime_error("Windows console input mode was not retained");
            }

            outerOutputMode = originalOutputMode | ENABLE_PROCESSED_OUTPUT |
                ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            if (!::SetConsoleMode(outputHandle, outerOutputMode))
                throw std::runtime_error("Could not configure Windows console output");
            outputModeChanged = true;
            if (!::GetConsoleMode(outputHandle, &actualMode) ||
                (actualMode & (ENABLE_PROCESSED_OUTPUT |
                               ENABLE_VIRTUAL_TERMINAL_PROCESSING)) !=
                    (ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                throw std::runtime_error("Windows console output mode was not retained");
            }
            outerOutputMode = actualMode;

            if (!::SetConsoleOutputCP(CP_UTF8))
                throw std::runtime_error("Could not configure Windows console encoding");
            codePageChanged = true;
            if (::GetConsoleOutputCP() != CP_UTF8)
                throw std::runtime_error("Windows console encoding was not retained");

            pasteEnabled = true;
            writeUtf8("\x1b[?2004h");
            registerControlHandler();
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~WindowsState() noexcept { shutdown(); }

    ConsoleEvent readEvent(ConsoleInput& owner) {
        std::size_t recordsProcessed = 0;
        while (true) {
            if (const auto urgent = urgentEvent(owner)) {
                discardDecoder(owner);
                return *urgent;
            }

            if (repeatRemaining != 0) {
                --repeatRemaining;
                owner.preferBufferedInput = false;
                if (const auto urgent = urgentEvent(owner)) {
                    discardDecoder(owner);
                    return *urgent;
                }
                return repeatText.empty()
                    ? ConsoleEvent(repeatKey)
                    : ConsoleEvent(ConsoleKey::Text, repeatText);
            }

            if (owner.workPending.load(std::memory_order_acquire) &&
                (!owner.preferBufferedInput || !inputReady())) {
                owner.workPending.exchange(false, std::memory_order_acq_rel);
                ::ResetEvent(workEvent);
                owner.preferBufferedInput = true;
                if (const auto urgent = urgentEvent(owner)) {
                    discardDecoder(owner);
                    return *urgent;
                }
                return ConsoleEvent(ConsoleKey::WorkAvailable);
            }

            if (owner.preferBufferedInput && !hasBufferedRecord() && inputReady()) {
                if (!readRecords()) {
                    discardDecoder(owner);
                    return ConsoleEvent(ConsoleKey::Failure);
                }
            }

            if (const auto parsed = parseBuffered(owner, false)) {
                owner.preferBufferedInput = false;
                if (const auto urgent = urgentEvent(owner)) {
                    discardDecoder(owner);
                    return *urgent;
                }
                return *parsed;
            }

            if (hasBufferedRecord()) {
                INPUT_RECORD record{};
                if (hasDeferredRecord) {
                    record = deferredRecord;
                    hasDeferredRecord = false;
                } else {
                    record = recordBatch[recordCursor++];
                }
                if (const auto event = processRecord(owner, record)) {
                    owner.preferBufferedInput = false;
                    if (const auto urgent = urgentEvent(owner)) {
                        discardDecoder(owner);
                        return *urgent;
                    }
                    return *event;
                }
                ++recordsProcessed;
                if (recordsProcessed >= recordBatch.size() &&
                    owner.workPending.load(std::memory_order_acquire)) {
                    owner.preferBufferedInput = false;
                }
                continue;
            }

            DWORD timeout = INFINITE;
            if (owner.escapePending) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    owner.escapeDeadline - std::chrono::steady_clock::now()).count();
                timeout = remaining > 0 ? static_cast<DWORD>(remaining) : 0;
            }
            const ConsoleKey wake = waitForInput(owner, timeout);
            if (wake != ConsoleKey::Text) {
                if (wake == ConsoleKey::Interrupted || wake == ConsoleKey::Failure)
                    discardDecoder(owner);
                return ConsoleEvent(wake);
            }
            if (owner.escapePending &&
                std::chrono::steady_clock::now() >= owner.escapeDeadline) {
                if (const auto parsed = parseBuffered(owner, true)) {
                    if (const auto urgent = urgentEvent(owner)) {
                        discardDecoder(owner);
                        return *urgent;
                    }
                    return *parsed;
                }
            }
        }
    }

    bool beginLineEcho() noexcept {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!::GetConsoleScreenBufferInfo(outputHandle, &info)) return false;
        lineOrigin = info.dwCursorPosition;
        lastEchoCells = 0;
        lineEchoActive = true;
        return true;
    }

    bool echoLine(const std::string& line) noexcept {
        try {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (!::GetConsoleScreenBufferInfo(outputHandle, &info)) return false;
            const LONG availableLong = static_cast<LONG>(info.srWindow.Right) -
                static_cast<LONG>(lineOrigin.X) + 1;
            const std::size_t available = availableLong > 0
                ? static_cast<std::size_t>(availableLong) : 0;
            const std::string safe = protected_chat::terminal_detail::safeText(line);
            std::wstring visible = protected_chat::terminal_detail::utf8ToUtf16(safe);
            std::size_t visibleCells = protected_chat::terminal_detail::displayWidth(safe);
            if (visibleCells > available) {
                const std::size_t prefixWidth = available >= 3 ? 3 : 0;
                const std::size_t suffixLimit = available - prefixWidth;
                std::size_t used = 0;
                std::size_t offset = visible.size();
                while (offset > 0 && used < suffixLimit) {
                    std::size_t previous = offset - 1;
                    if (lowSurrogate(visible[previous]) && previous > 0 &&
                        highSurrogate(visible[previous - 1])) --previous;
                    const std::wstring scalar = visible.substr(previous, offset - previous);
                    const std::string utf8 =
                        protected_chat::terminal_detail::utf16ToUtf8(scalar);
                    const std::size_t width =
                        protected_chat::terminal_detail::displayWidth(utf8);
                    if (width > suffixLimit - used) break;
                    used += width;
                    offset = previous;
                }
                visible = std::wstring(prefixWidth, L'.') + visible.substr(offset);
                visibleCells = used + prefixWidth;
            }
            if (!::SetConsoleCursorPosition(outputHandle, lineOrigin)) return false;
            if (!writeWide(visible)) return false;
            if (lastEchoCells > visibleCells) {
                const std::size_t erase = std::min(
                    lastEchoCells - visibleCells, available - visibleCells);
                if (!writeWide(std::wstring(erase, L' ')))
                    return false;
            }
            COORD cursor = lineOrigin;
            const LONG target = static_cast<LONG>(lineOrigin.X) +
                static_cast<LONG>(visibleCells);
            cursor.X = static_cast<SHORT>(std::min<LONG>(
                target, std::max<LONG>(0, info.dwSize.X - 1)));
            if (!::SetConsoleCursorPosition(outputHandle, cursor)) return false;
            lastEchoCells = visibleCells;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool finishLineEcho() noexcept {
        lineEchoActive = false;
        return writeWide(L"\r\n");
    }

    void releaseLineEcho() noexcept { lineEchoActive = false; }

    void signalWork() noexcept {
        if (workEvent == nullptr || !::SetEvent(workEvent)) {
            failureLatched.store(true, std::memory_order_release);
            if (stopEvent != nullptr) ::SetEvent(stopEvent);
        }
    }

    void signalStop() noexcept {
        if (stopEvent == nullptr || !::SetEvent(stopEvent)) {
            failureLatched.store(true, std::memory_order_release);
            if (workEvent != nullptr) ::SetEvent(workEvent);
        }
    }

    void enableResize() {
        if (resizeEnabled) return;
        DWORD current = 0;
        if (!::GetConsoleMode(inputHandle, &current))
            throw std::runtime_error("Could not read Windows resize mode");
        resizeInputMode = current;
        if (!::SetConsoleMode(inputHandle, current | ENABLE_WINDOW_INPUT))
            throw std::runtime_error("Could not enable Windows resize events");
        DWORD actual = 0;
        if (!::GetConsoleMode(inputHandle, &actual) ||
            (actual & ENABLE_WINDOW_INPUT) == 0) {
            ::SetConsoleMode(inputHandle, resizeInputMode);
            throw std::runtime_error("Windows resize mode was not retained");
        }
        resizeEnabled = true;
    }

    void disableResize() noexcept {
        if (!resizeEnabled) return;
        ::SetConsoleMode(inputHandle, resizeInputMode);
        resizeEnabled = false;
    }

private:
    std::optional<ConsoleEvent> urgentEvent(ConsoleInput& owner) {
        if (failureLatched.load(std::memory_order_acquire))
            return ConsoleEvent(ConsoleKey::Failure);
        if (stopPending(owner)) return ConsoleEvent(ConsoleKey::Interrupted);
        return std::nullopt;
    }

    bool stopPending(ConsoleInput& owner) noexcept {
        if (owner.interrupted.load(std::memory_order_acquire)) return true;
        const DWORD result = ::WaitForSingleObject(stopEvent, 0);
        if (result == WAIT_OBJECT_0) {
            owner.interrupted.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    bool inputReady() const noexcept {
        return hasBufferedRecord() || !vtBuffer.empty() ||
            repeatRemaining != 0 ||
            ::WaitForSingleObject(inputHandle, 0) == WAIT_OBJECT_0;
    }

    bool hasBufferedRecord() const noexcept {
        return hasDeferredRecord || recordCursor < recordCount;
    }

    bool readRecords() noexcept {
        if (!::ReadConsoleInputW(
                inputHandle, recordBatch.data(),
                static_cast<DWORD>(recordBatch.size()), &recordCount)) {
            failureLatched.store(true, std::memory_order_release);
            return false;
        }
        recordCursor = 0;
        return true;
    }

    ConsoleKey waitForInput(ConsoleInput& owner, DWORD timeout) noexcept {
        HANDLE handles[] = {stopEvent, workEvent, inputHandle};
        const DWORD result = ::WaitForMultipleObjects(3, handles, FALSE, timeout);
        if (result == WAIT_OBJECT_0) {
            owner.interrupted.store(true, std::memory_order_release);
            return ConsoleKey::Interrupted;
        }
        if (result == WAIT_OBJECT_0 + 1) {
            if (owner.workPending.exchange(false, std::memory_order_acq_rel)) {
                owner.preferBufferedInput = true;
                return ConsoleKey::WorkAvailable;
            }
            return ConsoleKey::Text;
        }
        if (result == WAIT_OBJECT_0 + 2) {
            if (!readRecords()) return ConsoleKey::Failure;
            return ConsoleKey::Text;
        }
        if (result == WAIT_TIMEOUT) return ConsoleKey::Text;
        failureLatched.store(true, std::memory_order_release);
        return ConsoleKey::Failure;
    }

    std::optional<ConsoleEvent> processRecord(
        ConsoleInput& owner, const INPUT_RECORD& record) {
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            if (resizeEnabled) return ConsoleEvent(ConsoleKey::Redraw);
            return std::nullopt;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
            return std::nullopt;

        const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
        const wchar_t character = key.uChar.UnicodeChar;
        const WORD repeats = key.wRepeatCount == 0 ? 1 : key.wRepeatCount;

        if (pendingHighSurrogate != 0) {
            if (lowSurrogate(character) && repeats == pendingHighRepeat) {
                std::wstring pair;
                pair.push_back(pendingHighSurrogate);
                pair.push_back(character);
                pendingHighSurrogate = 0;
                pendingHighRepeat = 0;
                try {
                    repeatText = protected_chat::terminal_detail::utf16ToUtf8(pair);
                } catch (...) {
                    repeatText.clear();
                    return ConsoleEvent(ConsoleKey::UnsupportedKey);
                }
                repeatKey = ConsoleKey::Text;
                repeatRemaining = repeats - 1;
                return ConsoleEvent(ConsoleKey::Text, repeatText);
            }
            pendingHighSurrogate = 0;
            pendingHighRepeat = 0;
            deferredRecord = record;
            hasDeferredRecord = true;
            return ConsoleEvent(ConsoleKey::UnsupportedKey);
        }

        if (owner.pasteActive || character == L'\x1b' || !vtBuffer.empty()) {
            if (character == 0) {
                if (owner.pasteActive) pasteMalformed = true;
                else if (!vtBuffer.empty()) {
                    owner.escapePending = false;
                    deferredRecord = record;
                    hasDeferredRecord = true;
                    vtBuffer.erase(0, 1);
                    return ConsoleEvent(ConsoleKey::Escape);
                }
                return std::nullopt;
            }
            if (!owner.pasteActive && vtBuffer.size() == 1 &&
                vtBuffer[0] == L'\x1b' && character != L'[' &&
                character != L'O') {
                owner.escapePending = false;
                vtBuffer.clear();
                deferredRecord = record;
                hasDeferredRecord = true;
                return ConsoleEvent(ConsoleKey::Escape);
            }
            if (vtBuffer.empty() && character == L'\x1b') {
                owner.escapePending = true;
                owner.escapeDeadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(50);
            }
            if (!owner.pasteActive && vtBuffer.size() + repeats > 32) {
                vtBuffer.append(32 - std::min<std::size_t>(vtBuffer.size(), 32),
                                character);
            } else {
                vtBuffer.append(repeats, character);
            }
            return parseBuffered(owner, false);
        }

        if (highSurrogate(character)) {
            pendingHighSurrogate = character;
            pendingHighRepeat = repeats;
            return std::nullopt;
        }
        if (lowSurrogate(character))
            return ConsoleEvent(ConsoleKey::UnsupportedKey);

        const DWORD modifiers = key.dwControlKeyState;
        const bool control = (modifiers & (LEFT_CTRL_PRESSED |
                                            RIGHT_CTRL_PRESSED)) != 0;
        const bool alt = (modifiers & (LEFT_ALT_PRESSED |
                                        RIGHT_ALT_PRESSED)) != 0;
        const bool altGr = (modifiers & RIGHT_ALT_PRESSED) != 0 &&
            (modifiers & LEFT_CTRL_PRESSED) != 0;
        ConsoleKey mapped = ConsoleKey::UnsupportedKey;
        bool repeatMapped = true;
        if (character == L'\r' || character == L'\n') {
            mapped = ConsoleKey::Enter;
            repeatMapped = false;
        } else if (character == 0x08 || character == 0x7f) {
            mapped = ConsoleKey::Backspace;
        } else if (character == 0x0e) mapped = ConsoleKey::Notifications;
        else if (character == 0x11 || character == 0x03 ||
                 character == 0x04) mapped = ConsoleKey::Exit;
        else if (character == 0x0c) mapped = ConsoleKey::Redraw;
        else if (character == 0x15) mapped = ConsoleKey::ClearInput;
        else if (character == 0x12) mapped = ConsoleKey::RestoreFailed;
        else if (character == 0x1a) mapped = ConsoleKey::EndOfFile;
        else if (control && !altGr && key.wVirtualKeyCode == VK_END) mapped = ConsoleKey::FollowLatest;
        else if (control && !altGr && key.wVirtualKeyCode == 'N') mapped = ConsoleKey::Notifications;
        else if (control && !altGr && (key.wVirtualKeyCode == 'Q' ||
                             key.wVirtualKeyCode == 'C' ||
                             key.wVirtualKeyCode == 'D')) mapped = ConsoleKey::Exit;
        else if (control && !altGr && key.wVirtualKeyCode == 'L') mapped = ConsoleKey::Redraw;
        else if (control && !altGr && key.wVirtualKeyCode == 'U') mapped = ConsoleKey::ClearInput;
        else if (control && !altGr && key.wVirtualKeyCode == 'R') mapped = ConsoleKey::RestoreFailed;
        else if (control && !altGr && key.wVirtualKeyCode == 'Z') mapped = ConsoleKey::EndOfFile;
        else if (key.wVirtualKeyCode == VK_RETURN) {
            mapped = ConsoleKey::Enter;
            repeatMapped = false;
        } else if (key.wVirtualKeyCode == VK_BACK) mapped = ConsoleKey::Backspace;
        else if (key.wVirtualKeyCode == VK_DELETE) mapped = ConsoleKey::Delete;
        else if (key.wVirtualKeyCode == VK_LEFT) mapped = ConsoleKey::Left;
        else if (key.wVirtualKeyCode == VK_RIGHT) mapped = ConsoleKey::Right;
        else if (key.wVirtualKeyCode == VK_HOME) mapped = ConsoleKey::Home;
        else if (key.wVirtualKeyCode == VK_END) mapped = ConsoleKey::End;
        else if (key.wVirtualKeyCode == VK_PRIOR) mapped = ConsoleKey::PageUp;
        else if (key.wVirtualKeyCode == VK_NEXT) mapped = ConsoleKey::PageDown;
        else if (key.wVirtualKeyCode == VK_ESCAPE) {
            vtBuffer.assign(1, L'\x1b');
            owner.escapePending = true;
            owner.escapeDeadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(50);
            return std::nullopt;
        } else if (key.wVirtualKeyCode == VK_TAB ||
                   (character != 0 &&
                    (character < 0x20 ||
                     (character >= 0x7f && character <= 0x9f)))) {
            mapped = ConsoleKey::UnsupportedKey;
        } else if (character >= 0x20 && character != 0x7f &&
                   ((!control && !alt) || altGr)) {
            try {
                repeatText = protected_chat::terminal_detail::utf16ToUtf8(
                    std::wstring(1, character));
            } catch (...) {
                repeatText.clear();
                return ConsoleEvent(ConsoleKey::UnsupportedKey);
            }
            repeatKey = ConsoleKey::Text;
            repeatRemaining = repeats - 1;
            return ConsoleEvent(ConsoleKey::Text, repeatText);
        }
        if (repeatMapped && repeats > 1) {
            repeatKey = mapped;
            repeatText.clear();
            repeatRemaining = repeats - 1;
        }
        return ConsoleEvent(mapped);
    }

    std::optional<ConsoleEvent> parseBuffered(
        ConsoleInput& owner, bool deadlineExpired) {
        static const std::wstring pasteStart = L"\x1b[200~";
        static const std::wstring pasteEnd = L"\x1b[201~";
        while (true) {
            if (owner.pasteActive) {
                const std::size_t end = vtBuffer.find(pasteEnd);
                if (end != std::wstring::npos) {
                    appendPaste(owner, vtBuffer.substr(0, end));
                    vtBuffer.erase(0, end + pasteEnd.size());
                    if (pasteHighSurrogate != 0) {
                        pasteMalformed = true;
                        pasteHighSurrogate = 0;
                    }
                    owner.pasteActive = false;
                    owner.escapePending = false;
                    if (owner.pasteTooLarge) {
                        resetPaste(owner);
                        return ConsoleEvent(ConsoleKey::InputTooLarge);
                    }
                    if (pasteMalformed) {
                        resetPaste(owner);
                        pasteRejectionPending = true;
                        return ConsoleEvent(ConsoleKey::UnsupportedKey);
                    }
                    std::string result = std::move(owner.pasteBuffer);
                    resetPaste(owner);
                    return ConsoleEvent(ConsoleKey::Paste, std::move(result));
                }
                std::size_t retained = 0;
                const std::size_t maximum = std::min(
                    vtBuffer.size(), pasteEnd.size() - 1);
                for (std::size_t length = maximum; length > 0; --length) {
                    if (vtBuffer.compare(
                            vtBuffer.size() - length, length,
                            pasteEnd, 0, length) == 0) {
                        retained = length;
                        break;
                    }
                }
                const std::size_t safe = vtBuffer.size() - retained;
                if (safe != 0) {
                    appendPaste(owner, vtBuffer.substr(0, safe));
                    vtBuffer.erase(0, safe);
                }
                return std::nullopt;
            }

            if (vtBuffer.empty()) return std::nullopt;
            if (vtBuffer[0] != L'\x1b') {
                const wchar_t character = vtBuffer[0];
                vtBuffer.erase(0, 1);
                if (character < 0x20 ||
                    (character >= 0x7f && character <= 0x9f)) {
                    return ConsoleEvent(ConsoleKey::UnsupportedKey);
                }
                if (highSurrogate(character)) {
                    if (vtBuffer.empty() || !lowSurrogate(vtBuffer[0]))
                        return ConsoleEvent(ConsoleKey::UnsupportedKey);
                    const std::wstring pair{character, vtBuffer[0]};
                    vtBuffer.erase(0, 1);
                    try {
                        return ConsoleEvent(
                            ConsoleKey::Text,
                            protected_chat::terminal_detail::utf16ToUtf8(pair));
                    } catch (...) {
                        return ConsoleEvent(ConsoleKey::UnsupportedKey);
                    }
                }
                if (lowSurrogate(character))
                    return ConsoleEvent(ConsoleKey::UnsupportedKey);
                try {
                    return ConsoleEvent(
                        ConsoleKey::Text,
                        protected_chat::terminal_detail::utf16ToUtf8(
                            std::wstring(1, character)));
                } catch (...) {
                    return ConsoleEvent(ConsoleKey::UnsupportedKey);
                }
            }

            if (vtBuffer.size() == 1) {
                if (!deadlineExpired) return std::nullopt;
                vtBuffer.erase(0, 1);
                owner.escapePending = false;
                return ConsoleEvent(ConsoleKey::Escape);
            }

            const std::size_t compared = std::min(vtBuffer.size(), pasteStart.size());
            if (vtBuffer.compare(0, compared, pasteStart, 0, compared) == 0) {
                if (vtBuffer.size() < pasteStart.size()) {
                    if (!deadlineExpired) return std::nullopt;
                    vtBuffer.clear();
                    owner.escapePending = false;
                    return ConsoleEvent(ConsoleKey::UnsupportedKey);
                }
                vtBuffer.erase(0, pasteStart.size());
                owner.pasteActive = true;
                owner.pasteTooLarge = false;
                owner.pasteBuffer.clear();
                pasteMalformed = false;
                pasteHighSurrogate = 0;
                owner.escapePending = false;
                continue;
            }

            if (vtBuffer[1] != L'[' && vtBuffer[1] != L'O') {
                vtBuffer.erase(0, 1);
                owner.escapePending = false;
                return ConsoleEvent(ConsoleKey::Escape);
            }
            std::size_t end = 2;
            while (end < vtBuffer.size() && end < 32) {
                const wchar_t character = vtBuffer[end];
                if (character >= 0x40 && character <= 0x7e) break;
                ++end;
            }
            if (end < vtBuffer.size() && end < 32) {
                const std::wstring sequence = vtBuffer.substr(0, end + 1);
                vtBuffer.erase(0, end + 1);
                owner.escapePending = false;
                return ConsoleEvent(vtKey(sequence));
            }
            if (vtBuffer.size() >= 32) {
                vtBuffer.erase(0, 32);
                owner.escapePending = false;
                return ConsoleEvent(ConsoleKey::Failure);
            }
            if (deadlineExpired) {
                vtBuffer.clear();
                owner.escapePending = false;
                return ConsoleEvent(ConsoleKey::UnsupportedKey);
            }
            return std::nullopt;
        }
    }

    void appendPaste(ConsoleInput& owner, const std::wstring& text) {
        for (wchar_t character : text) {
            if (pasteHighSurrogate != 0) {
                if (!lowSurrogate(character)) {
                    pasteMalformed = true;
                    pasteHighSurrogate = 0;
                } else {
                    appendPasteScalar(owner,
                        std::wstring{pasteHighSurrogate, character}, 4);
                    pasteHighSurrogate = 0;
                    continue;
                }
            }
            if (highSurrogate(character)) {
                pasteHighSurrogate = character;
            } else if (lowSurrogate(character)) {
                pasteMalformed = true;
            } else {
                const std::size_t bytes = character < 0x80 ? 1 :
                    (character < 0x800 ? 2 : 3);
                appendPasteScalar(owner, std::wstring(1, character), bytes);
            }
        }
    }

    void appendPasteScalar(
        ConsoleInput& owner, const std::wstring& scalar, std::size_t bytes) {
        if (owner.pasteTooLarge) return;
        if (bytes > ConsoleInput::MaxPasteBytes - owner.pasteBuffer.size()) {
            owner.pasteTooLarge = true;
            owner.pasteBuffer.clear();
            return;
        }
        try {
            owner.pasteBuffer += protected_chat::terminal_detail::utf16ToUtf8(scalar);
        } catch (...) {
            pasteMalformed = true;
        }
    }

    void resetPaste(ConsoleInput& owner) noexcept {
        owner.pasteBuffer.clear();
        owner.pasteTooLarge = false;
        pasteMalformed = false;
        pasteHighSurrogate = 0;
    }

    void discardDecoder(ConsoleInput& owner) noexcept {
        recordCount = 0;
        recordCursor = 0;
        hasDeferredRecord = false;
        vtBuffer.clear();
        repeatText.clear();
        repeatRemaining = 0;
        pendingHighSurrogate = 0;
        pendingHighRepeat = 0;
        owner.pasteActive = false;
        owner.escapePending = false;
        pasteRejectionPending = false;
        resetPaste(owner);
    }

public:
    bool consumePasteRejection() noexcept {
        const bool rejected = pasteRejectionPending;
        pasteRejectionPending = false;
        return rejected;
    }

private:

    bool writeWide(const std::wstring& text) noexcept {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const std::size_t count = std::min<std::size_t>(
                16'384, text.size() - offset);
            DWORD written = 0;
            if (!::WriteConsoleW(
                    outputHandle, text.data() + offset,
                    static_cast<DWORD>(count), &written, nullptr) ||
                written == 0) return false;
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    void writeUtf8(const std::string& text) {
        const std::wstring converted =
            protected_chat::terminal_detail::utf8ToUtf16(text);
        if (!writeWide(converted))
            throw std::runtime_error("Windows console output failed");
    }

    void registerControlHandler() {
        ::AcquireSRWLockExclusive(&registrationLock);
        if (registrationActive) {
            ::ReleaseSRWLockExclusive(&registrationLock);
            throw std::runtime_error("Another Windows console input is active");
        }
        registeredStopEvent = stopEvent;
        registeredCleanupEvent = cleanupCompleteEvent;
        registeredHandlersIdleEvent = handlersIdleEvent;
        activeHandlers = 0;
        registrationActive = true;
        registrationPublished = true;
        ::ReleaseSRWLockExclusive(&registrationLock);
        if (!::SetConsoleCtrlHandler(consoleControlHandler, TRUE)) {
            ::AcquireSRWLockExclusive(&registrationLock);
            registrationActive = false;
            registeredStopEvent = nullptr;
            registeredCleanupEvent = nullptr;
            registeredHandlersIdleEvent = nullptr;
            registrationPublished = false;
            ::ReleaseSRWLockExclusive(&registrationLock);
            throw std::runtime_error("Could not install Windows console control handling");
        }
        handlerRegistered = true;
    }

    void shutdown() noexcept {
        if (shutdownComplete) return;
        shutdownComplete = true;
        disableResize();
        if (pasteEnabled) {
            try { writeUtf8("\x1b[?2004l"); }
            catch (...) {}
            pasteEnabled = false;
        }
        if (codePageChanged) ::SetConsoleOutputCP(originalOutputCodePage);
        if (outputModeChanged) ::SetConsoleMode(outputHandle, originalOutputMode);
        if (inputModeChanged) ::SetConsoleMode(inputHandle, originalInputMode);

        if (registrationPublished) {
            ::AcquireSRWLockExclusive(&registrationLock);
            registrationActive = false;
            ::ReleaseSRWLockExclusive(&registrationLock);
            if (cleanupCompleteEvent != nullptr) ::SetEvent(cleanupCompleteEvent);
        }
        if (handlerRegistered) {
            ::SetConsoleCtrlHandler(consoleControlHandler, FALSE);
            handlerRegistered = false;
        }

        bool handlersIdle = true;
        if (registrationPublished && handlersIdleEvent != nullptr)
            handlersIdle = ::WaitForSingleObject(
                handlersIdleEvent, INFINITE) == WAIT_OBJECT_0;
        if (registrationPublished) {
            ::AcquireSRWLockExclusive(&registrationLock);
            registeredStopEvent = nullptr;
            registeredCleanupEvent = nullptr;
            registeredHandlersIdleEvent = nullptr;
            registrationPublished = false;
            ::ReleaseSRWLockExclusive(&registrationLock);
        }
        if (!handlersIdle) {
            stopEvent = workEvent = cleanupCompleteEvent = handlersIdleEvent = nullptr;
            return;
        }
        if (handlersIdleEvent != nullptr) ::CloseHandle(handlersIdleEvent);
        if (cleanupCompleteEvent != nullptr) ::CloseHandle(cleanupCompleteEvent);
        if (workEvent != nullptr) ::CloseHandle(workEvent);
        if (stopEvent != nullptr) ::CloseHandle(stopEvent);
        handlersIdleEvent = cleanupCompleteEvent = workEvent = stopEvent = nullptr;
    }

    HANDLE inputHandle = INVALID_HANDLE_VALUE;
    HANDLE outputHandle = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    HANDLE workEvent = nullptr;
    HANDLE cleanupCompleteEvent = nullptr;
    HANDLE handlersIdleEvent = nullptr;
    DWORD originalInputMode = 0;
    DWORD originalOutputMode = 0;
    DWORD originalOutputCodePage = 0;
    DWORD outerInputMode = 0;
    DWORD outerOutputMode = 0;
    DWORD resizeInputMode = 0;
    bool inputModeChanged = false;
    bool outputModeChanged = false;
    bool codePageChanged = false;
    bool pasteEnabled = false;
    bool resizeEnabled = false;
    bool handlerRegistered = false;
    bool registrationPublished = false;
    bool shutdownComplete = false;
    std::atomic<bool> failureLatched{false};
    std::array<INPUT_RECORD, 64> recordBatch{};
    DWORD recordCount = 0;
    DWORD recordCursor = 0;
    INPUT_RECORD deferredRecord{};
    bool hasDeferredRecord = false;
    std::wstring vtBuffer;
    wchar_t pendingHighSurrogate = 0;
    WORD pendingHighRepeat = 0;
    wchar_t pasteHighSurrogate = 0;
    bool pasteMalformed = false;
    bool pasteRejectionPending = false;
    ConsoleKey repeatKey = ConsoleKey::Text;
    std::string repeatText;
    WORD repeatRemaining = 0;
    COORD lineOrigin{};
    std::size_t lastEchoCells = 0;
    bool lineEchoActive = false;
};

ConsoleInput::ConsoleInput()
    : interrupted(false), workPending(false), endOfFileSeen(false),
      pasteActive(false), pasteTooLarge(false), preferBufferedInput(true),
      escapePending(false), windowsState(std::make_unique<WindowsState>()) {}

ConsoleInput::~ConsoleInput() noexcept {
    windowsState.reset();
    inputBuffer.clear();
    pasteBuffer.clear();
    pasteActive = false;
    escapePending = false;
}

ConsoleReadResult ConsoleInput::readLine(std::string& line) {
    line.clear();
    if (!windowsState || !windowsState->beginLineEcho() ||
        !windowsState->echoLine(inputBuffer)) return ConsoleReadResult::Failure;
    while (true) {
        const ConsoleEvent event = windowsState->readEvent(*this);
        switch (event.key()) {
            case ConsoleKey::Text:
                if (event.text().size() > MaxLineBytes - inputBuffer.size())
                    return ConsoleReadResult::TooLong;
                inputBuffer += event.text();
                if (!windowsState->echoLine(inputBuffer))
                    return ConsoleReadResult::Failure;
                break;
            case ConsoleKey::Paste:
                if (pasteContainsControl(event.text()))
                    return ConsoleReadResult::Failure;
                if (event.text().size() > MaxLineBytes - inputBuffer.size())
                    return ConsoleReadResult::TooLong;
                inputBuffer += event.text();
                if (!windowsState->echoLine(inputBuffer))
                    return ConsoleReadResult::Failure;
                break;
            case ConsoleKey::Backspace:
                removeLastUtf8Scalar(inputBuffer);
                if (!windowsState->echoLine(inputBuffer))
                    return ConsoleReadResult::Failure;
                break;
            case ConsoleKey::ClearInput:
                inputBuffer.clear();
                if (!windowsState->echoLine(inputBuffer))
                    return ConsoleReadResult::Failure;
                break;
            case ConsoleKey::Enter:
                line = std::move(inputBuffer);
                inputBuffer.clear();
                return windowsState->finishLineEcho()
                    ? ConsoleReadResult::Line : ConsoleReadResult::Failure;
            case ConsoleKey::WorkAvailable:
                windowsState->releaseLineEcho();
                return ConsoleReadResult::WorkAvailable;
            case ConsoleKey::Interrupted:
            case ConsoleKey::Exit:
                return ConsoleReadResult::Interrupted;
            case ConsoleKey::EndOfFile:
                if (inputBuffer.empty()) return ConsoleReadResult::EndOfFile;
                break;
            case ConsoleKey::InputTooLarge:
                return ConsoleReadResult::TooLong;
            case ConsoleKey::Failure:
                return ConsoleReadResult::Failure;
            case ConsoleKey::Unsupported:
                return ConsoleReadResult::Failure;
            case ConsoleKey::UnsupportedKey:
                if (windowsState->consumePasteRejection())
                    return ConsoleReadResult::Failure;
                break;
            case ConsoleKey::Escape:
                break;
            case ConsoleKey::Delete:
            case ConsoleKey::Left:
            case ConsoleKey::Right:
            case ConsoleKey::Home:
            case ConsoleKey::End:
            case ConsoleKey::PageUp:
            case ConsoleKey::PageDown:
            case ConsoleKey::FollowLatest:
            case ConsoleKey::Notifications:
            case ConsoleKey::RestoreFailed:
                break;
            case ConsoleKey::Redraw:
                if (!windowsState->echoLine(inputBuffer))
                    return ConsoleReadResult::Failure;
                break;
        }
    }
}

ConsoleEvent ConsoleInput::readEvent() {
    return windowsState ? windowsState->readEvent(*this)
                        : ConsoleEvent(ConsoleKey::Failure);
}

void ConsoleInput::notifyWork() noexcept {
    workPending.store(true, std::memory_order_release);
    if (windowsState) windowsState->signalWork();
}

void ConsoleInput::interrupt() noexcept {
    interrupted.store(true, std::memory_order_release);
    if (windowsState) windowsState->signalStop();
}

void ConsoleInput::enableResizeNotifications() {
    if (!windowsState) throw std::runtime_error("Windows console input is unavailable");
    windowsState->enableResize();
}

void ConsoleInput::disableResizeNotifications() noexcept {
    if (windowsState) windowsState->disableResize();
}

#endif
