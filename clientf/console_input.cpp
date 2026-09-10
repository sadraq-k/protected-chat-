#include "console_input.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {
#ifndef _WIN32
volatile sig_atomic_t resizeWriteDescriptor = -1;

void resizeSignalHandler(int) {
    const int savedErrno = errno;
    const int descriptor = static_cast<int>(resizeWriteDescriptor);
    if (descriptor >= 0) {
        const char byte = 'R';
        ssize_t result;
        do {
            result = ::write(descriptor, &byte, 1);
        } while (result == -1 && errno == EINTR);
    }
    errno = savedErrno;
}

std::size_t utf8Length(unsigned char first) {
    if (first < 0x80u) return 1;
    if (first >= 0xc2u && first <= 0xdfu) return 2;
    if (first >= 0xe0u && first <= 0xefu) return 3;
    if (first >= 0xf0u && first <= 0xf4u) return 4;
    return 0;
}

bool validUtf8Sequence(const std::string& text, std::size_t length) {
    if (text.size() < length) return false;
    for (std::size_t index = 1; index < length; ++index) {
        if ((static_cast<unsigned char>(text[index]) & 0xc0u) != 0x80u) {
            return false;
        }
    }
    if (length > 1) {
        const unsigned char first = static_cast<unsigned char>(text[0]);
        const unsigned char second = static_cast<unsigned char>(text[1]);
        if ((first == 0xe0u && second < 0xa0u) ||
            (first == 0xedu && second >= 0xa0u) ||
            (first == 0xf0u && second < 0x90u) ||
            (first == 0xf4u && second >= 0x90u)) return false;
    }
    return true;
}
#endif
} // namespace

ConsoleEvent::ConsoleEvent(ConsoleKey key, std::string text)
    : eventKey(key), eventText(std::move(text)) {}
ConsoleKey ConsoleEvent::key() const noexcept { return eventKey; }
const std::string& ConsoleEvent::text() const noexcept { return eventText; }

ConsoleInput::ConsoleInput()
    : interrupted(false), workPending(false), endOfFileSeen(false),
      pasteActive(false), pasteTooLarge(false), preferBufferedInput(true),
      escapePending(false)
#ifndef _WIN32
      , wakeReadDescriptor(-1), wakeWriteDescriptor(-1),
      resizeNotificationsEnabled(false), previousResizeActionValid(false),
      previousResizeActionStorage{}
#endif
{
#ifndef _WIN32
    static_assert(sizeof(previousResizeActionStorage) >= sizeof(struct sigaction),
                  "SIGWINCH storage is too small");
    int descriptors[2];
    if (::pipe(descriptors) != 0) {
        throw std::runtime_error("Could not create the console wakeup pipe");
    }
    wakeReadDescriptor = descriptors[0];
    wakeWriteDescriptor = descriptors[1];
    const int readFlags = ::fcntl(wakeReadDescriptor, F_GETFL, 0);
    const int writeFlags = ::fcntl(wakeWriteDescriptor, F_GETFL, 0);
    if (readFlags == -1 || writeFlags == -1 ||
        ::fcntl(wakeReadDescriptor, F_SETFL, readFlags | O_NONBLOCK) == -1 ||
        ::fcntl(wakeWriteDescriptor, F_SETFL, writeFlags | O_NONBLOCK) == -1) {
        ::close(wakeReadDescriptor);
        ::close(wakeWriteDescriptor);
        wakeReadDescriptor = wakeWriteDescriptor = -1;
        throw std::runtime_error("Could not configure the console wakeup pipe");
    }
#endif
}

ConsoleInput::~ConsoleInput() noexcept {
#ifndef _WIN32
    disableResizeNotifications();
    if (wakeReadDescriptor != -1) ::close(wakeReadDescriptor);
    if (wakeWriteDescriptor != -1) ::close(wakeWriteDescriptor);
#endif
}

ConsoleReadResult ConsoleInput::readLine(std::string& line) {
    line.clear();
#ifdef _WIN32
    return ConsoleReadResult::Unsupported;
#else
    std::array<char, 4096> chunk{};
    while (true) {
        if (interrupted.load(std::memory_order_acquire))
            return ConsoleReadResult::Interrupted;
        if (workPending.exchange(false, std::memory_order_acq_rel))
            return ConsoleReadResult::WorkAvailable;
        const std::size_t newline = inputBuffer.find('\n');
        if (newline != std::string::npos) {
            if (newline > MaxLineBytes) return ConsoleReadResult::TooLong;
            line.assign(inputBuffer.data(), newline);
            inputBuffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return ConsoleReadResult::Line;
        }
        if (inputBuffer.size() > MaxLineBytes) return ConsoleReadResult::TooLong;
        if (endOfFileSeen) {
            if (inputBuffer.empty()) return ConsoleReadResult::EndOfFile;
            line = std::move(inputBuffer);
            inputBuffer.clear();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return ConsoleReadResult::Line;
        }
        pollfd descriptors[2] = {
            {STDIN_FILENO, POLLIN, 0}, {wakeReadDescriptor, POLLIN, 0}};
        const int ready = ::poll(descriptors, 2, -1);
        if (ready == -1) {
            if (errno == EINTR) continue;
            return ConsoleReadResult::Failure;
        }
        if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
            while (::read(wakeReadDescriptor, chunk.data(), chunk.size()) > 0) {}
            if (interrupted.load(std::memory_order_acquire))
                return ConsoleReadResult::Interrupted;
            if (workPending.exchange(false, std::memory_order_acq_rel))
                return ConsoleReadResult::WorkAvailable;
        }
        if ((descriptors[1].revents & (POLLERR | POLLNVAL)) != 0)
            return ConsoleReadResult::Failure;
        if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
            const std::size_t remaining = MaxLineBytes + 1 - inputBuffer.size();
            const ssize_t count = ::read(
                STDIN_FILENO, chunk.data(), std::min(chunk.size(), remaining));
            if (count > 0)
                inputBuffer.append(chunk.data(), static_cast<std::size_t>(count));
            else if (count == 0)
                endOfFileSeen = true;
            else if (errno != EINTR)
                return ConsoleReadResult::Failure;
        }
        if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0)
            return ConsoleReadResult::Failure;
    }
#endif
}

ConsoleEvent ConsoleInput::readEvent() {
#ifdef _WIN32
    return ConsoleEvent(ConsoleKey::Unsupported);
#else
    return readPosixEvent();
#endif
}

#ifndef _WIN32
bool ConsoleInput::fillEventInput(int timeout, ConsoleKey& wakeEvent) {
    std::array<char, 4096> chunk{};
    pollfd descriptors[2] = {
        {STDIN_FILENO, POLLIN, 0}, {wakeReadDescriptor, POLLIN, 0}};
    const int ready = ::poll(descriptors, 2, timeout);
    if (ready == -1) {
        if (errno == EINTR) return true;
        wakeEvent = ConsoleKey::Failure;
        return false;
    }
    if (ready == 0) return false;
    if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
        bool resize = false;
        while (true) {
            const ssize_t count = ::read(wakeReadDescriptor, chunk.data(), chunk.size());
            if (count <= 0) break;
            for (ssize_t index = 0; index < count; ++index)
                resize = resize || chunk[static_cast<std::size_t>(index)] == 'R';
        }
        if (interrupted.load(std::memory_order_acquire))
            wakeEvent = ConsoleKey::Interrupted;
        else if (resize)
            wakeEvent = ConsoleKey::Redraw;
        else {
            workPending.exchange(false, std::memory_order_acq_rel);
            wakeEvent = ConsoleKey::WorkAvailable;
        }
    }
    if ((descriptors[1].revents & (POLLERR | POLLNVAL)) != 0) {
        wakeEvent = ConsoleKey::Failure;
        return false;
    }
    if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
        const ssize_t count = ::read(STDIN_FILENO, chunk.data(), chunk.size());
        if (count > 0)
            inputBuffer.append(chunk.data(), static_cast<std::size_t>(count));
        else if (count == 0)
            endOfFileSeen = true;
        else if (errno != EINTR) {
            wakeEvent = ConsoleKey::Failure;
            return false;
        }
    }
    if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) {
        wakeEvent = ConsoleKey::Failure;
        return false;
    }
    return true;
}

ConsoleEvent ConsoleInput::readPosixEvent() {
    static const std::string pasteStart = "\x1b[200~";
    static const std::string pasteEnd = "\x1b[201~";
    while (true) {
        if (interrupted.load(std::memory_order_acquire))
            return ConsoleEvent(ConsoleKey::Interrupted);
        if (pasteActive) {
            const std::size_t end = inputBuffer.find(pasteEnd);
            if (end != std::string::npos) {
                if (!pasteTooLarge && pasteBuffer.size() + end <= MaxPasteBytes)
                    pasteBuffer.append(inputBuffer.data(), end);
                else
                    pasteTooLarge = true;
                inputBuffer.erase(0, end + pasteEnd.size());
                pasteActive = false;
                if (pasteTooLarge) {
                    pasteTooLarge = false;
                    pasteBuffer.clear();
                    return ConsoleEvent(ConsoleKey::InputTooLarge);
                }
                std::string result = std::move(pasteBuffer);
                pasteBuffer.clear();
                return ConsoleEvent(ConsoleKey::Paste, std::move(result));
            }
            if (inputBuffer.size() > pasteEnd.size()) {
                const std::size_t safe = inputBuffer.size() - pasteEnd.size();
                if (!pasteTooLarge && pasteBuffer.size() + safe <= MaxPasteBytes)
                    pasteBuffer.append(inputBuffer.data(), safe);
                else
                    pasteTooLarge = true;
                inputBuffer.erase(0, safe);
            }
        }
        if (workPending.load(std::memory_order_acquire) &&
            (!preferBufferedInput || inputBuffer.empty())) {
            workPending.exchange(false, std::memory_order_acq_rel);
            preferBufferedInput = true;
            return ConsoleEvent(ConsoleKey::WorkAvailable);
        }
        if (!pasteActive && !inputBuffer.empty()) {
            preferBufferedInput = false;
            const unsigned char first = static_cast<unsigned char>(inputBuffer[0]);
            if (first == 0x1bu) {
                if (inputBuffer.size() >= pasteStart.size() &&
                    inputBuffer.compare(0, pasteStart.size(), pasteStart) == 0) {
                    inputBuffer.erase(0, pasteStart.size());
                    pasteActive = true;
                    pasteTooLarge = false;
                    pasteBuffer.clear();
                    escapePending = false;
                    continue;
                }
                if (!escapePending) {
                    escapePending = true;
                    escapeDeadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(50);
                }
                if (inputBuffer.size() == 1) {
                    if (std::chrono::steady_clock::now() >= escapeDeadline) {
                        inputBuffer.erase(0, 1);
                        escapePending = false;
                        return ConsoleEvent(ConsoleKey::Escape);
                    }
                } else if (inputBuffer[1] != '[' && inputBuffer[1] != 'O') {
                    inputBuffer.erase(0, 1);
                    escapePending = false;
                    return ConsoleEvent(ConsoleKey::Escape);
                } else {
                    std::size_t end = 2;
                    while (end < inputBuffer.size() && end < 32) {
                        const unsigned char byte = static_cast<unsigned char>(inputBuffer[end]);
                        if (byte >= 0x40u && byte <= 0x7eu) break;
                        ++end;
                    }
                    if (end < inputBuffer.size() && end < 32) {
                        const std::string sequence = inputBuffer.substr(0, end + 1);
                        inputBuffer.erase(0, end + 1);
                        escapePending = false;
                        if (sequence == "\x1b[D" || sequence == "\x1bOD") return ConsoleEvent(ConsoleKey::Left);
                        if (sequence == "\x1b[C" || sequence == "\x1bOC") return ConsoleEvent(ConsoleKey::Right);
                        if (sequence == "\x1b[H" || sequence == "\x1b[1~" || sequence == "\x1bOH") return ConsoleEvent(ConsoleKey::Home);
                        if (sequence == "\x1b[F" || sequence == "\x1b[4~" || sequence == "\x1bOF") return ConsoleEvent(ConsoleKey::End);
                        if (sequence == "\x1b[3~") return ConsoleEvent(ConsoleKey::Delete);
                        if (sequence == "\x1b[5~") return ConsoleEvent(ConsoleKey::PageUp);
                        if (sequence == "\x1b[6~") return ConsoleEvent(ConsoleKey::PageDown);
                        if (sequence == "\x1b[1;5F") return ConsoleEvent(ConsoleKey::FollowLatest);
                        return ConsoleEvent(ConsoleKey::UnsupportedKey);
                    }
                    if (inputBuffer.size() >= 32) {
                        inputBuffer.erase(0, 32);
                        escapePending = false;
                        return ConsoleEvent(ConsoleKey::UnsupportedKey);
                    }
                }
            } else {
                inputBuffer.erase(0, 1);
                switch (first) {
                    case '\r': case '\n': return ConsoleEvent(ConsoleKey::Enter);
                    case 0x7f: case 0x08: return ConsoleEvent(ConsoleKey::Backspace);
                    case 0x0e: return ConsoleEvent(ConsoleKey::Notifications);
                    case 0x11: case 0x03: case 0x04: return ConsoleEvent(ConsoleKey::Exit);
                    case 0x0c: return ConsoleEvent(ConsoleKey::Redraw);
                    case 0x15: return ConsoleEvent(ConsoleKey::ClearInput);
                    case 0x12: return ConsoleEvent(ConsoleKey::RestoreFailed);
                    case '\t': return ConsoleEvent(ConsoleKey::UnsupportedKey);
                    default: break;
                }
                if (first < 0x20u) return ConsoleEvent(ConsoleKey::UnsupportedKey);
                const std::size_t length = utf8Length(first);
                if (length == 0) return ConsoleEvent(ConsoleKey::UnsupportedKey);
                inputBuffer.insert(inputBuffer.begin(), static_cast<char>(first));
                if (inputBuffer.size() >= length) {
                    if (!validUtf8Sequence(inputBuffer, length)) {
                        inputBuffer.erase(0, 1);
                        return ConsoleEvent(ConsoleKey::UnsupportedKey);
                    }
                    std::string text = inputBuffer.substr(0, length);
                    inputBuffer.erase(0, length);
                    return ConsoleEvent(ConsoleKey::Text, std::move(text));
                }
            }
        }
        if (endOfFileSeen && inputBuffer.empty() && !pasteActive)
            return ConsoleEvent(ConsoleKey::EndOfFile);
        int timeout = -1;
        if (escapePending) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                escapeDeadline - std::chrono::steady_clock::now());
            timeout = static_cast<int>(std::max<std::int64_t>(0, remaining.count()));
        }
        ConsoleKey wake = ConsoleKey::Text;
        const bool ready = fillEventInput(timeout, wake);
        if (wake != ConsoleKey::Text) {
            preferBufferedInput = true;
            return ConsoleEvent(wake);
        }
        if (!ready && escapePending) {
            inputBuffer.erase(0, 1);
            escapePending = false;
            return ConsoleEvent(ConsoleKey::Escape);
        }
    }
}

void ConsoleInput::writeWakeByte(char byte) noexcept {
    if (wakeWriteDescriptor == -1) return;
    ssize_t result;
    do {
        result = ::write(wakeWriteDescriptor, &byte, 1);
    } while (result == -1 && errno == EINTR);
}
#endif

void ConsoleInput::notifyWork() noexcept {
    bool expected = false;
    if (!workPending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) return;
#ifndef _WIN32
    writeWakeByte('W');
#endif
}

void ConsoleInput::interrupt() noexcept {
    bool expected = false;
    if (!interrupted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) return;
#ifndef _WIN32
    writeWakeByte('I');
#endif
}

void ConsoleInput::enableResizeNotifications() {
#ifdef _WIN32
    throw std::runtime_error("Full-screen terminal input is unsupported on Windows");
#else
    if (resizeNotificationsEnabled) return;
    if (resizeWriteDescriptor != -1)
        throw std::runtime_error("Another console owns resize notifications");
    if (wakeWriteDescriptor > std::numeric_limits<sig_atomic_t>::max())
        throw std::runtime_error("Console descriptor cannot receive resize events");
    struct sigaction action{};
    action.sa_handler = resizeSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    auto* previous = reinterpret_cast<struct sigaction*>(previousResizeActionStorage);
    resizeWriteDescriptor = static_cast<sig_atomic_t>(wakeWriteDescriptor);
    if (::sigaction(SIGWINCH, &action, previous) != 0) {
        resizeWriteDescriptor = -1;
        throw std::runtime_error("Could not install terminal resize handling");
    }
    previousResizeActionValid = true;
    resizeNotificationsEnabled = true;
#endif
}

void ConsoleInput::disableResizeNotifications() noexcept {
#ifndef _WIN32
    if (!resizeNotificationsEnabled) return;
    resizeWriteDescriptor = -1;
    if (previousResizeActionValid) {
        const auto* previous = reinterpret_cast<const struct sigaction*>(previousResizeActionStorage);
        ::sigaction(SIGWINCH, previous, nullptr);
    }
    previousResizeActionValid = false;
    resizeNotificationsEnabled = false;
#endif
}
