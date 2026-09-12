#include "console_input.h"

#include <utility>

ConsoleEvent::ConsoleEvent(ConsoleKey key, std::string text)
    : eventKey(key), eventText(std::move(text)) {}
ConsoleKey ConsoleEvent::key() const noexcept { return eventKey; }
const std::string& ConsoleEvent::text() const noexcept { return eventText; }
