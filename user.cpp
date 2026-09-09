#include "clientf/client_connection.h"
#include "clientf/console_input.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>

using json = nlohmann::json;

#ifndef PROTECTED_CHAT_CLIENT_HOST
#define PROTECTED_CHAT_CLIENT_HOST "194.9.56.182"
#endif

#ifndef PROTECTED_CHAT_CLIENT_PORT
#define PROTECTED_CHAT_CLIENT_PORT "1403"
#endif

namespace {

class ConsoleOutput {
public:
    void line(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        std::cout << text << std::endl;
    }

    void error(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        std::cerr << text << std::endl;
    }

    void prompt(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        std::cout << text << std::flush;
    }

private:
    std::mutex mutex;
};

std::string safeDisplayText(const std::string& text) {
    constexpr char HexDigits[] = "0123456789ABCDEF";
    std::string safe;
    safe.reserve(text.size());
    for (const unsigned char character : text) {
        if (character < 0x20 || character == 0x7f) {
            safe += "\\x";
            safe.push_back(HexDigits[character >> 4]);
            safe.push_back(HexDigits[character & 0x0f]);
        } else {
            safe.push_back(static_cast<char>(character));
        }
    }
    return safe;
}

bool readConsoleLine(
    ConsoleInput& input,
    ConsoleOutput& output,
    ClientConnection& connection,
    const std::string& prompt,
    std::string& line) {
    output.prompt(prompt);
    ConsoleReadResult result = input.readLine(line);
    while (result == ConsoleReadResult::WorkAvailable) {
        if (!connection.flushAutomaticRequests()) {
            return false;
        }
        result = input.readLine(line);
    }
    if (result == ConsoleReadResult::Line) {
        return true;
    }
    if (result == ConsoleReadResult::Interrupted) {
        return false;
    }
    if (result == ConsoleReadResult::EndOfFile) {
        output.line("[CLIENT] Standard input closed");
    } else if (result == ConsoleReadResult::TooLong) {
        output.error("[ERROR] Console input exceeded the 1 MiB limit");
        connection.requestStop("Console input too long");
    } else if (result == ConsoleReadResult::Unsupported) {
        output.error(
            "[ERROR] Interruptible console input is not implemented on Windows");
        connection.requestStop("Unsupported console input platform");
    } else {
        output.error("[ERROR] Console input failed");
        connection.requestStop("Console input failed");
    }
    return false;
}

void finishConnection(
    ClientConnection& connection,
    const std::string& reason) {
    connection.requestStop(reason);
    connection.joinReceiver();
    connection.close();
}

bool sendRequest(
    ClientConnection& connection,
    ConsoleOutput& output,
    const json& request,
    const std::string& label) {
    const ClientSendResult result = connection.sendJson(request);
    if (result == ClientSendResult::Written) {
        output.line("[SEND] " + label + " frame written");
        return true;
    }
    if (result == ClientSendResult::TooLarge) {
        output.error(
            "[ERROR] " + label +
            " exceeds the server's 65,536-byte request limit");
        return false;
    }
    output.error("[ERROR] " + label + " write failed");
    return false;
}

bool buildAuthenticationRequest(
    ConsoleInput& input,
    ConsoleOutput& output,
    ClientConnection& connection,
    json& request) {
    std::string choice;
    if (!readConsoleLine(
            input,
            output,
            connection,
            "Enter 1 for sign in, 2 for log in: ",
            choice)) {
        return false;
    }

    if (choice == "1") {
        std::string name;
        std::string username;
        std::string password;
        if (!readConsoleLine(
                input, output, connection, "Enter name: ", name) ||
            !readConsoleLine(
                input, output, connection, "Enter username: ", username) ||
            !readConsoleLine(
                input, output, connection, "Enter password: ", password)) {
            return false;
        }
        if (name.empty() || username.empty() || password.empty()) {
            output.error("[ERROR] All fields must be filled");
            connection.requestStop("Incomplete registration input");
            return false;
        }
        output.line("[AUTH] Registration requested for: " + username);
        request = {
            {"action", "SIGN_IN"},
            {"name", name},
            {"username", username},
            {"password", password}
        };
        return true;
    }

    if (choice == "2") {
        std::string username;
        std::string password;
        if (!readConsoleLine(
                input, output, connection, "Enter username: ", username) ||
            !readConsoleLine(
                input, output, connection, "Enter password: ", password)) {
            return false;
        }
        if (username.empty() || password.empty()) {
            output.error("[ERROR] Username and password required");
            connection.requestStop("Incomplete login input");
            return false;
        }
        output.line("[AUTH] Login requested for: " + username);
        request = {
            {"action", "LOG_IN"},
            {"username", username},
            {"password", password}
        };
        return true;
    }

    output.error("[ERROR] Invalid choice");
    connection.requestStop("Invalid authentication choice");
    return false;
}

bool parseIntegerInRange(
    const std::string& text,
    std::int64_t minimum,
    std::int64_t maximum,
    std::int64_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc() &&
        parsed.ptr == text.data() + text.size() &&
        value >= minimum && value <= maximum;
}

bool buildGroupListRequest(
    const std::string& input,
    ConsoleOutput& output,
    json& message) {
    if (input == "GROUP_LIST") {
        message = {{"type", "GROUP_LIST"}};
        return true;
    }
    const std::string arguments = input.substr(11);
    const std::size_t delimiter = arguments.find(':');
    if (delimiter == std::string::npos ||
        arguments.find(':', delimiter + 1) != std::string::npos) {
        output.error("[ERROR] Invalid group-list arguments");
        return false;
    }
    std::int64_t afterGroupId = 0;
    std::int64_t limit = 0;
    if (!parseIntegerInRange(
            arguments.substr(0, delimiter),
            0,
            std::numeric_limits<std::int64_t>::max(),
            afterGroupId) ||
        !parseIntegerInRange(
            arguments.substr(delimiter + 1), 1, 100, limit)) {
        output.error("[ERROR] Invalid group-list arguments");
        return false;
    }
    message = {
        {"type", "GROUP_LIST"},
        {"after_group_id", afterGroupId},
        {"limit", limit}
    };
    return true;
}

bool buildContactRequest(
    const std::string& input,
    ConsoleOutput& output,
    json& message) {
    if (input.rfind("CONTACT_ADD:", 0) == 0) {
        const std::string username = input.substr(12);
        if (username.empty()) {
            output.error("[ERROR] Invalid contact-add arguments");
            return false;
        }
        message = {{"type", "CONTACT_ADD"}, {"username", username}};
        return true;
    }
    if (input == "CONTACT_ADD") {
        output.error("[ERROR] Invalid contact-add arguments");
        return false;
    }
    if (input == "CONTACT_LIST") {
        message = {{"type", "CONTACT_LIST"}};
        return true;
    }
    if (input.rfind("CONTACT_LIST:", 0) == 0) {
        const std::string arguments = input.substr(13);
        const std::size_t delimiter = arguments.find(':');
        if (delimiter == std::string::npos ||
            arguments.find(':', delimiter + 1) != std::string::npos) {
            output.error("[ERROR] Invalid contact-list arguments");
            return false;
        }
        std::int64_t afterUserId = 0;
        std::int64_t limit = 0;
        if (!parseIntegerInRange(
                arguments.substr(0, delimiter),
                0,
                std::numeric_limits<std::int64_t>::max(),
                afterUserId) ||
            !parseIntegerInRange(
                arguments.substr(delimiter + 1), 1, 100, limit)) {
            output.error("[ERROR] Invalid contact-list arguments");
            return false;
        }
        message = {
            {"type", "CONTACT_LIST"},
            {"after_user_id", afterUserId},
            {"limit", limit}
        };
        return true;
    }
    return false;
}

bool buildSearchRequest(
    const std::string& input,
    ConsoleOutput& output,
    const std::string& operation,
    const std::string& simplePrefix,
    const std::string& pagePrefix,
    const char* cursorField,
    json& message) {
    if (input.rfind(simplePrefix, 0) == 0) {
        const std::string query = input.substr(simplePrefix.size());
        if (query.empty()) {
            output.error("[ERROR] Invalid search query");
            return false;
        }
        message = {
            {"type", operation},
            {"query", query},
            {cursorField, 0},
            {"limit", 50}
        };
        return true;
    }
    if (input.rfind(pagePrefix, 0) == 0) {
        const std::string arguments = input.substr(pagePrefix.size());
        const std::size_t first = arguments.find(':');
        const std::size_t second = first == std::string::npos
            ? std::string::npos : arguments.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            output.error("[ERROR] Invalid search-page arguments");
            return false;
        }
        std::int64_t cursor = 0;
        std::int64_t limit = 0;
        const std::string query = arguments.substr(second + 1);
        if (!parseIntegerInRange(
                arguments.substr(0, first),
                0,
                std::numeric_limits<std::int64_t>::max(),
                cursor) ||
            !parseIntegerInRange(
                arguments.substr(first + 1, second - first - 1),
                1,
                100,
                limit) ||
            query.empty()) {
            output.error("[ERROR] Invalid search-page arguments");
            return false;
        }
        message = {
            {"type", operation},
            {"query", query},
            {cursorField, cursor},
            {"limit", limit}
        };
        return true;
    }
    if (input == operation || input == operation + "_PAGE") {
        output.error("[ERROR] Invalid search arguments");
        return false;
    }
    return false;
}

bool buildHistoryRequest(
    const std::string& input,
    ConsoleOutput& output,
    json& message) {
    if (input == "HISTORY") {
        message = {
            {"type", "HISTORY"},
            {"after_message_id", 0},
            {"limit", 50}
        };
        return true;
    }
    const std::string arguments = input.substr(8);
    const std::size_t first = arguments.find(':');
    const std::size_t second = first == std::string::npos
        ? std::string::npos : arguments.find(':', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        arguments.find(':', second + 1) != std::string::npos) {
        output.error("[ERROR] Invalid history arguments");
        return false;
    }
    std::int64_t afterMessageId = 0;
    std::int64_t throughMessageId = 0;
    std::int64_t limit = 0;
    if (!parseIntegerInRange(
            arguments.substr(0, first),
            0,
            std::numeric_limits<std::int64_t>::max(),
            afterMessageId) ||
        !parseIntegerInRange(
            arguments.substr(first + 1, second - first - 1),
            afterMessageId,
            std::numeric_limits<std::int64_t>::max(),
            throughMessageId) ||
        !parseIntegerInRange(arguments.substr(second + 1), 1, 50, limit)) {
        output.error("[ERROR] Invalid history arguments");
        return false;
    }
    message = {
        {"type", "HISTORY"},
        {"after_message_id", afterMessageId},
        {"through_message_id", throughMessageId},
        {"limit", limit}
    };
    return true;
}

bool buildMessageRequest(
    const std::string& input,
    ConsoleOutput& output,
    json& message) {
    if (input.rfind("PRIVATE:", 0) == 0) {
        const std::size_t delimiter = input.find(':', 8);
        const std::string receiver = delimiter == std::string::npos
            ? std::string() : input.substr(8, delimiter - 8);
        const std::string content = delimiter == std::string::npos
            ? std::string() : input.substr(delimiter + 1);
        if (receiver.empty() || content.empty()) {
            output.error("[ERROR] Invalid private message format");
            return false;
        }
        message = {
            {"type", "PRIVATE"},
            {"receiver", receiver},
            {"content", content}
        };
        return true;
    }

    if (input.rfind("GROUP:", 0) == 0) {
        const std::size_t delimiter = input.find(':', 6);
        const std::string idText = delimiter == std::string::npos
            ? std::string() : input.substr(6, delimiter - 6);
        const std::string content = delimiter == std::string::npos
            ? std::string() : input.substr(delimiter + 1);
        std::int64_t groupId = 0;
        if (!parseIntegerInRange(
                idText,
                1,
                std::numeric_limits<std::int64_t>::max(),
                groupId) ||
            content.empty()) {
            output.error("[ERROR] Invalid group message format");
            return false;
        }
        message = {
            {"type", "GROUP"},
            {"group_id", groupId},
            {"content", content}
        };
        return true;
    }

    if (input.rfind("BROADCAST:", 0) == 0) {
        const std::string content = input.substr(10);
        if (content.empty()) {
            output.error("[ERROR] Broadcast content cannot be empty");
            return false;
        }
        message = {{"type", "BROADCAST"}, {"content", content}};
        return true;
    }

    if (input.rfind("GROUP_CREATE:", 0) == 0) {
        const std::string name = input.substr(13);
        if (name.empty()) {
            output.error("[ERROR] Group name cannot be empty");
            return false;
        }
        message = {{"type", "GROUP_CREATE"}, {"name", name}};
        return true;
    }

    if (input.rfind("GROUP_JOIN:", 0) == 0) {
        const std::string idText = input.substr(11);
        std::int64_t groupId = 0;
        if (!parseIntegerInRange(
                idText,
                1,
                std::numeric_limits<std::int64_t>::max(),
                groupId)) {
            output.error("[ERROR] Invalid group ID");
            return false;
        }
        message = {{"type", "GROUP_JOIN"}, {"group_id", groupId}};
        return true;
    }

    if (input == "GROUP_LIST" || input.rfind("GROUP_LIST:", 0) == 0) {
        return buildGroupListRequest(input, output, message);
    }

    if (input == "CONTACT_ADD" || input.rfind("CONTACT_ADD:", 0) == 0 ||
        input == "CONTACT_LIST" || input.rfind("CONTACT_LIST:", 0) == 0) {
        return buildContactRequest(input, output, message);
    }

    if (input == "USER_SEARCH" || input == "USER_SEARCH_PAGE" ||
        input.rfind("USER_SEARCH:", 0) == 0 ||
        input.rfind("USER_SEARCH_PAGE:", 0) == 0) {
        return buildSearchRequest(
            input,
            output,
            "USER_SEARCH",
            "USER_SEARCH:",
            "USER_SEARCH_PAGE:",
            "after_user_id",
            message);
    }

    if (input == "GROUP_SEARCH" || input == "GROUP_SEARCH_PAGE" ||
        input.rfind("GROUP_SEARCH:", 0) == 0 ||
        input.rfind("GROUP_SEARCH_PAGE:", 0) == 0) {
        return buildSearchRequest(
            input,
            output,
            "GROUP_SEARCH",
            "GROUP_SEARCH:",
            "GROUP_SEARCH_PAGE:",
            "after_group_id",
            message);
    }

    if (input == "HISTORY" || input.rfind("HISTORY:", 0) == 0) {
        return buildHistoryRequest(input, output, message);
    }

    output.error(
        "[ERROR] Use messaging, group, contact, search, HISTORY, SYNC, or exit commands");
    return false;
}

void runClient(const std::string& host, const std::string& port) {
    ConsoleInput input;
    ConsoleOutput output;
    ClientConnection connection;
    std::atomic<bool> historyRequestOutstanding(false);

    connection.setAutomaticRequestNotifier([&input] { input.notifyWork(); });

    output.line("[CLIENT] Connecting to " + host + ":" + port);
    try {
        connection.connect(host, port);
    } catch (const boost::system::system_error& error) {
        output.error(
            "[ERROR] Connection failed: " + error.code().message());
        return;
    }
    output.line("[CLIENT] Connected to server");

    try {
        connection.startReceiver(
            [&output, &historyRequestOutstanding](
                ClientFrameKind kind,
                const json& message) {
                if (kind == ClientFrameKind::Message) {
                    std::string prefix =
                        "[MESSAGE #" +
                        std::to_string(message.at("message_id").get<std::int64_t>()) +
                        " " + message.at("kind").get<std::string>() +
                        "] " + message.at("sender").get<std::string>();
                    if (message.at("kind").get<std::string>() == "GROUP") {
                        prefix += " in group " + std::to_string(
                            message.at("group_id").get<std::int64_t>());
                    }
                    output.line(
                        prefix + ": " +
                        message.at("content").get<std::string>());
                    return;
                }

                const std::string status =
                    message.at("status").get<std::string>();
                const std::string detail =
                    message.at("message").get<std::string>();
                const auto operation = message.find("operation");
                const std::string operationName =
                    operation != message.end() && operation->is_string()
                    ? operation->get<std::string>() : std::string();
                if (operationName == "HISTORY") {
                    historyRequestOutstanding.store(
                        false, std::memory_order_release);
                    if (status == "SUCCESS") {
                        for (const json& record : message.at("messages")) {
                            std::string prefix =
                                "[HISTORY #" + std::to_string(
                                    record.at("message_id").get<std::int64_t>()) +
                                " " + record.at("kind").get<std::string>() +
                                "] " + record.at("sender").get<std::string>();
                            if (record.at("kind").get<std::string>() == "GROUP") {
                                prefix += " in group " + std::to_string(
                                    record.at("group_id").get<std::int64_t>());
                            }
                            output.line(
                                prefix + ": " +
                                record.at("content").get<std::string>());
                        }
                        output.line(
                            "[HISTORY] through=" + std::to_string(
                                message.at("through_message_id").get<std::int64_t>()) +
                            " next=" + std::to_string(
                                message.at("next_after_message_id").get<std::int64_t>()) +
                            " has_more=" +
                            (message.at("has_more").get<bool>() ? "true" : "false"));
                        return;
                    }
                }
                if (operationName == "CONTACT_ADD" && status == "SUCCESS") {
                    const json& contact = message.at("contact");
                    output.line(
                        "[CONTACT " + std::to_string(
                            contact.at("user_id").get<std::int64_t>()) +
                        "] " + safeDisplayText(
                            contact.at("username").get<std::string>()) +
                        " (" + message.at("code").get<std::string>() + ")");
                    return;
                }
                if (operationName == "CONTACT_LIST" && status == "SUCCESS") {
                    for (const json& contact : message.at("contacts")) {
                        output.line(
                            "[CONTACT " + std::to_string(
                                contact.at("user_id").get<std::int64_t>()) +
                            "] " + safeDisplayText(
                                contact.at("username").get<std::string>()));
                    }
                    output.line(
                        "[CONTACT_LIST] next=" + std::to_string(
                            message.at("next_after_user_id").get<std::int64_t>()) +
                        " has_more=" +
                        (message.at("has_more").get<bool>() ? "true" : "false"));
                    return;
                }
                if (operationName == "USER_SEARCH" && status == "SUCCESS") {
                    output.line(
                        "[USER_SEARCH] query=" + safeDisplayText(
                            message.at("query").get<std::string>()));
                    for (const json& user : message.at("users")) {
                        output.line(
                            "[USER " + std::to_string(
                                user.at("user_id").get<std::int64_t>()) +
                            "] " + safeDisplayText(
                                user.at("username").get<std::string>()) +
                            " contact=" +
                            (user.at("is_contact").get<bool>()
                                ? "true" : "false"));
                    }
                    output.line(
                        "[USER_SEARCH] next=" + std::to_string(
                            message.at("next_after_user_id").get<std::int64_t>()) +
                        " has_more=" +
                        (message.at("has_more").get<bool>() ? "true" : "false"));
                    return;
                }
                if (operationName == "GROUP_SEARCH" && status == "SUCCESS") {
                    output.line(
                        "[GROUP_SEARCH] query=" + safeDisplayText(
                            message.at("query").get<std::string>()));
                    for (const json& group : message.at("groups")) {
                        output.line(
                            "[GROUP " + std::to_string(
                                group.at("group_id").get<std::int64_t>()) +
                            "] " + safeDisplayText(
                                group.at("name").get<std::string>()) +
                            " member=" +
                            (group.at("is_member").get<bool>()
                                ? "true" : "false"));
                    }
                    output.line(
                        "[GROUP_SEARCH] next=" + std::to_string(
                            message.at("next_after_group_id").get<std::int64_t>()) +
                        " has_more=" +
                        (message.at("has_more").get<bool>() ? "true" : "false"));
                    return;
                }
                if (operationName == "GROUP_LIST" && status == "SUCCESS") {
                    for (const json& group : message.at("groups")) {
                        output.line(
                            "[GROUP " + std::to_string(
                                group.at("id").get<std::int64_t>()) +
                            "] " + group.at("name").get<std::string>());
                    }
                    output.line(
                        "[GROUP_LIST] next=" + std::to_string(
                            message.at("next_after_group_id").get<std::int64_t>()) +
                        " has_more=" +
                        (message.at("has_more").get<bool>() ? "true" : "false"));
                    return;
                }
                output.line("Server [" + status + "]: " + detail);
            },
            [&output](const std::string& message, bool error) {
                if (error) {
                    output.error("[ERROR] " + message);
                } else {
                    output.line("[CLIENT] " + message);
                }
            },
            [&input] { input.interrupt(); });
    } catch (const std::system_error& error) {
        output.error(
            "[ERROR] Could not start receiver: " +
            std::string(error.what()));
        finishConnection(connection, "Receiver thread startup failed");
        return;
    }

    json authenticationRequest;
    if (!buildAuthenticationRequest(
            input, output, connection, authenticationRequest)) {
        finishConnection(connection, "Authentication input ended");
        return;
    }
    if (!sendRequest(
            connection,
            output,
            authenticationRequest,
            "Authentication request")) {
        finishConnection(connection, "Authentication request was not written");
        return;
    }

    std::string authenticationMessage;
    const AuthenticationResult authenticationResult =
        connection.waitForAuthentication(authenticationMessage);
    if (authenticationResult != AuthenticationResult::Success) {
        output.line("Authentication failed: " + authenticationMessage);
        finishConnection(connection, "Authentication did not succeed");
        return;
    }

    output.line("Authentication successful! " + authenticationMessage);
    connection.beginPostAuthentication();
    if (!connection.startPendingSynchronization()) {
        finishConnection(connection, "Initial pending synchronization failed");
        return;
    }

    while (!connection.isStopping()) {
        if (!connection.flushAutomaticRequests()) {
            break;
        }
        std::string command;
        if (!readConsoleLine(
                input,
                output,
                connection,
                "Command (PRIVATE/GROUP/BROADCAST/GROUP_CREATE/GROUP_JOIN/"
                "GROUP_LIST/CONTACT/SEARCH/HISTORY/SYNC/exit): ",
                command)) {
            if (connection.isAuthenticated() && !connection.isStopping()) {
                sendRequest(connection, output, {{"type", "EXIT"}}, "EXIT");
            }
            break;
        }
        if (command.empty()) {
            continue;
        }
        if (command == "exit") {
            sendRequest(connection, output, {{"type", "EXIT"}}, "EXIT");
            break;
        }
        if (command == "SYNC") {
            if (!connection.startPendingSynchronization()) {
                output.error(
                    "[ERROR] Pending synchronization is already active");
            }
            continue;
        }

        json message;
        if (!buildMessageRequest(command, output, message)) {
            continue;
        }
        const bool isHistory = message.at("type").get<std::string>() == "HISTORY";
        if (isHistory) {
            bool expected = false;
            if (!historyRequestOutstanding.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                output.error("[ERROR] A HISTORY request is already outstanding");
                continue;
            }
        }
        const bool sent = sendRequest(connection, output, message, "Message");
        if (!sent && isHistory) {
            historyRequestOutstanding.store(false, std::memory_order_release);
        }
        if (!sent && connection.isStopping()) {
            break;
        }
    }

    finishConnection(connection, "Client stopped");
    output.line("[CLIENT] Connection closed");
}

} // namespace

int main() {
    try {
        runClient(PROTECTED_CHAT_CLIENT_HOST, PROTECTED_CHAT_CLIENT_PORT);
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] Client error: " << error.what() << std::endl;
        return 1;
    }
    return 0;
}
