#ifndef PROTECTED_CHAT_TERMINAL_PLATFORM_H
#define PROTECTED_CHAT_TERMINAL_PLATFORM_H

#include <cstdint>
#include <string>

namespace protected_chat::terminal_detail {

int platformCodePointWidth(std::uint32_t codePoint) noexcept;

#ifdef _WIN32
std::wstring utf8ToUtf16(const std::string& text);
std::string utf16ToUtf8(const std::wstring& text);
#endif

} // namespace protected_chat::terminal_detail

#endif
