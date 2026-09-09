#ifndef CONSOLE_INPUT_H
#define CONSOLE_INPUT_H

#include <atomic>
#include <cstddef>
#include <string>

enum class ConsoleReadResult {
    Line,
    WorkAvailable,
    EndOfFile,
    Interrupted,
    TooLong,
    Unsupported,
    Failure
};

class ConsoleInput {
public:
    static constexpr std::size_t MaxLineBytes = 1'048'576;

    ConsoleInput();
    ~ConsoleInput() noexcept;

    ConsoleInput(const ConsoleInput&) = delete;
    ConsoleInput& operator=(const ConsoleInput&) = delete;
    ConsoleInput(ConsoleInput&&) = delete;
    ConsoleInput& operator=(ConsoleInput&&) = delete;

    ConsoleReadResult readLine(std::string& line);
    void notifyWork() noexcept;
    void interrupt() noexcept;

private:
    std::string inputBuffer;
    std::atomic<bool> interrupted;
    std::atomic<bool> workPending;
    bool endOfFileSeen;
#ifndef _WIN32
    int wakeReadDescriptor;
    int wakeWriteDescriptor;
#endif
};

#endif
