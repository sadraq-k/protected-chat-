#include "console_input.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

ConsoleInput::ConsoleInput()
    : interrupted(false),
      workPending(false),
      endOfFileSeen(false)
#ifndef _WIN32
      , wakeReadDescriptor(-1),
      wakeWriteDescriptor(-1)
#endif
{
#ifndef _WIN32
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
        wakeReadDescriptor = -1;
        wakeWriteDescriptor = -1;
        throw std::runtime_error("Could not configure the console wakeup pipe");
    }
#endif
}

ConsoleInput::~ConsoleInput() noexcept {
#ifndef _WIN32
    if (wakeReadDescriptor != -1) {
        ::close(wakeReadDescriptor);
    }
    if (wakeWriteDescriptor != -1) {
        ::close(wakeWriteDescriptor);
    }
#endif
}

ConsoleReadResult ConsoleInput::readLine(std::string& line) {
    line.clear();
#ifdef _WIN32
    return ConsoleReadResult::Unsupported;
#else
    constexpr std::size_t ReadChunkBytes = 4096;
    std::array<char, ReadChunkBytes> chunk{};

    while (true) {
        if (interrupted.load(std::memory_order_acquire)) {
            return ConsoleReadResult::Interrupted;
        }
        if (workPending.exchange(false, std::memory_order_acq_rel)) {
            return ConsoleReadResult::WorkAvailable;
        }

        const std::size_t newline = inputBuffer.find('\n');
        if (newline != std::string::npos) {
            if (newline > MaxLineBytes) {
                return ConsoleReadResult::TooLong;
            }
            line.assign(inputBuffer.data(), newline);
            inputBuffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return ConsoleReadResult::Line;
        }

        if (inputBuffer.size() > MaxLineBytes) {
            return ConsoleReadResult::TooLong;
        }
        if (endOfFileSeen) {
            if (inputBuffer.empty()) {
                return ConsoleReadResult::EndOfFile;
            }
            line = std::move(inputBuffer);
            inputBuffer.clear();
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return ConsoleReadResult::Line;
        }
        pollfd descriptors[2] = {
            {STDIN_FILENO, POLLIN, 0},
            {wakeReadDescriptor, POLLIN, 0}
        };
        const int ready = ::poll(descriptors, 2, -1);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }
            return ConsoleReadResult::Failure;
        }

        if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
            while (::read(wakeReadDescriptor, chunk.data(), chunk.size()) > 0) {
            }
            if (interrupted.load(std::memory_order_acquire)) {
                return ConsoleReadResult::Interrupted;
            }
            if (workPending.exchange(false, std::memory_order_acq_rel)) {
                return ConsoleReadResult::WorkAvailable;
            }
            continue;
        }
        if ((descriptors[1].revents & (POLLERR | POLLNVAL)) != 0) {
            return ConsoleReadResult::Failure;
        }

        if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
            const std::size_t remainingUntilTooLong =
                MaxLineBytes + 1 - inputBuffer.size();
            const std::size_t requestedBytes =
                std::min(chunk.size(), remainingUntilTooLong);
            const ssize_t received =
                ::read(STDIN_FILENO, chunk.data(), requestedBytes);
            if (received > 0) {
                inputBuffer.append(
                    chunk.data(), static_cast<std::size_t>(received));
            } else if (received == 0) {
                endOfFileSeen = true;
            } else if (errno != EINTR) {
                return ConsoleReadResult::Failure;
            }
            continue;
        }
        if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) {
            return ConsoleReadResult::Failure;
        }
    }
#endif
}

void ConsoleInput::notifyWork() noexcept {
    bool expected = false;
    if (!workPending.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
#ifndef _WIN32
    if (wakeWriteDescriptor != -1) {
        const char signal = 1;
        const ssize_t ignored = ::write(wakeWriteDescriptor, &signal, 1);
        (void)ignored;
    }
#endif
}

void ConsoleInput::interrupt() noexcept {
    bool expected = false;
    if (!interrupted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }

#ifndef _WIN32
    if (wakeWriteDescriptor != -1) {
        const char signal = 1;
        const ssize_t ignored = ::write(wakeWriteDescriptor, &signal, 1);
        (void)ignored;
    }
#endif
}
