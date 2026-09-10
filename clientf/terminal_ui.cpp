#include "terminal_ui.h"

#include "console_input.h"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

using json = nlohmann::json;

namespace {

constexpr std::size_t PageLimit = 50;

bool isAsciiWhitespace(unsigned char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
        character == '\n' || character == '\f' || character == '\v';
}

std::string trimAsciiWhitespace(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() &&
           isAsciiWhitespace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first &&
           isAsciiWhitespace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    return text.substr(first, last - first);
}

bool isAsciiWhitespaceOnly(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return isAsciiWhitespace(character);
    });
}

bool parseUnsignedChoice(const std::string& input, std::size_t& value) {
    const std::string text = trimAsciiWhitespace(input);
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc() ||
        result.ptr != text.data() + text.size() ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

bool parsePositiveInt64(const std::string& input, std::int64_t& value) {
    const std::string text = trimAsciiWhitespace(input);
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc() ||
        result.ptr != text.data() + text.size() || parsed == 0 ||
        parsed > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    value = static_cast<std::int64_t>(parsed);
    return true;
}

bool readSignedInteger(
    const json& value,
    const char* field,
    std::int64_t& result) {
    const auto found = value.find(field);
    if (found == value.end()) {
        return false;
    }
    if (found->is_number_unsigned()) {
        const std::uint64_t parsed = found->get<std::uint64_t>();
        if (parsed > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        result = static_cast<std::int64_t>(parsed);
        return true;
    }
    if (!found->is_number_integer()) {
        return false;
    }
    result = found->get<std::int64_t>();
    return true;
}

bool readString(
    const json& value,
    const char* field,
    std::string& result) {
    const auto found = value.find(field);
    if (found == value.end() || !found->is_string()) {
        return false;
    }
    result = found->get<std::string>();
    return true;
}

bool isSuccessFor(const json& response, const std::string& operation) {
    std::string status;
    std::string actualOperation;
    std::string message;
    return response.is_object() &&
        readString(response, "status", status) && status == "SUCCESS" &&
        readString(response, "operation", actualOperation) &&
        actualOperation == operation &&
        readString(response, "message", message);
}

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

bool validContactPage(
    const json& response,
    std::int64_t afterUserId,
    std::int64_t& next,
    bool& hasMore) {
    if (!isSuccessFor(response, "CONTACT_LIST")) {
        return false;
    }
    const auto contacts = response.find("contacts");
    const auto more = response.find("has_more");
    if (contacts == response.end() || !contacts->is_array() ||
        contacts->size() > PageLimit || more == response.end() ||
        !more->is_boolean() ||
        !readSignedInteger(response, "next_after_user_id", next) ||
        next < afterUserId) {
        return false;
    }
    std::int64_t previous = afterUserId;
    for (const json& contact : *contacts) {
        std::int64_t id = 0;
        std::string username;
        if (!contact.is_object() ||
            !readSignedInteger(contact, "user_id", id) || id <= previous ||
            !readString(contact, "username", username)) {
            return false;
        }
        previous = id;
    }
    hasMore = more->get<bool>();
    return next == (contacts->empty() ? afterUserId : previous) &&
        (!hasMore || (!contacts->empty() && next > afterUserId));
}

bool validGroupListPage(
    const json& response,
    std::int64_t afterGroupId,
    std::int64_t& next,
    bool& hasMore) {
    if (!isSuccessFor(response, "GROUP_LIST")) {
        return false;
    }
    const auto groups = response.find("groups");
    const auto more = response.find("has_more");
    if (groups == response.end() || !groups->is_array() ||
        groups->size() > PageLimit || more == response.end() ||
        !more->is_boolean() ||
        !readSignedInteger(response, "next_after_group_id", next) ||
        next < afterGroupId) {
        return false;
    }
    std::int64_t previous = afterGroupId;
    for (const json& group : *groups) {
        std::int64_t id = 0;
        std::string name;
        if (!group.is_object() || !readSignedInteger(group, "id", id) ||
            id <= previous || !readString(group, "name", name)) {
            return false;
        }
        previous = id;
    }
    hasMore = more->get<bool>();
    return next == (groups->empty() ? afterGroupId : previous) &&
        (!hasMore || (!groups->empty() && next > afterGroupId));
}

bool validUserSearchPage(
    const json& response,
    const std::string& query,
    std::int64_t afterUserId,
    std::int64_t& next,
    bool& hasMore) {
    std::string echoedQuery;
    if (!isSuccessFor(response, "USER_SEARCH") ||
        !readString(response, "query", echoedQuery) || echoedQuery != query) {
        return false;
    }
    const auto users = response.find("users");
    const auto more = response.find("has_more");
    if (users == response.end() || !users->is_array() ||
        users->size() > PageLimit || more == response.end() ||
        !more->is_boolean() ||
        !readSignedInteger(response, "next_after_user_id", next) ||
        next < afterUserId) {
        return false;
    }
    std::int64_t previous = afterUserId;
    for (const json& user : *users) {
        std::int64_t id = 0;
        std::string username;
        const auto isContact = user.find("is_contact");
        if (!user.is_object() || !readSignedInteger(user, "user_id", id) ||
            id <= previous || !readString(user, "username", username) ||
            isContact == user.end() || !isContact->is_boolean()) {
            return false;
        }
        previous = id;
    }
    hasMore = more->get<bool>();
    return next == (users->empty() ? afterUserId : previous) &&
        (!hasMore || (!users->empty() && next > afterUserId));
}

bool validGroupSearchPage(
    const json& response,
    const std::string& query,
    std::int64_t afterGroupId,
    std::int64_t& next,
    bool& hasMore) {
    std::string echoedQuery;
    if (!isSuccessFor(response, "GROUP_SEARCH") ||
        !readString(response, "query", echoedQuery) || echoedQuery != query) {
        return false;
    }
    const auto groups = response.find("groups");
    const auto more = response.find("has_more");
    if (groups == response.end() || !groups->is_array() ||
        groups->size() > PageLimit || more == response.end() ||
        !more->is_boolean() ||
        !readSignedInteger(response, "next_after_group_id", next) ||
        next < afterGroupId) {
        return false;
    }
    std::int64_t previous = afterGroupId;
    for (const json& group : *groups) {
        std::int64_t id = 0;
        std::string name;
        const auto isMember = group.find("is_member");
        if (!group.is_object() ||
            !readSignedInteger(group, "group_id", id) || id <= previous ||
            !readString(group, "name", name) || isMember == group.end() ||
            !isMember->is_boolean()) {
            return false;
        }
        previous = id;
    }
    hasMore = more->get<bool>();
    return next == (groups->empty() ? afterGroupId : previous) &&
        (!hasMore || (!groups->empty() && next > afterGroupId));
}

bool validHistoryPage(
    const json& response,
    std::int64_t afterMessageId,
    const std::optional<std::int64_t>& expectedWatermark,
    std::int64_t& watermark,
    std::int64_t& next,
    bool& hasMore) {
    if (!isSuccessFor(response, "HISTORY") ||
        !readSignedInteger(response, "through_message_id", watermark) ||
        watermark < 0 || (expectedWatermark && watermark != *expectedWatermark) ||
        !readSignedInteger(response, "next_after_message_id", next) ||
        next < afterMessageId || next > watermark) {
        return false;
    }
    const auto messages = response.find("messages");
    const auto more = response.find("has_more");
    if (messages == response.end() || !messages->is_array() ||
        messages->size() > PageLimit || more == response.end() ||
        !more->is_boolean()) {
        return false;
    }
    std::int64_t previous = afterMessageId;
    for (const json& message : *messages) {
        std::int64_t id = 0;
        std::int64_t senderId = 0;
        std::int64_t createdAt = 0;
        std::string type;
        std::string kind;
        std::string sender;
        std::string content;
        if (!message.is_object() || !readString(message, "type", type) ||
            type != "MESSAGE" || !readSignedInteger(message, "message_id", id) ||
            id <= previous || id > watermark ||
            !readString(message, "kind", kind) ||
            (kind != "PRIVATE" && kind != "GROUP" && kind != "BROADCAST") ||
            !readSignedInteger(message, "sender_id", senderId) || senderId <= 0 ||
            !readString(message, "sender", sender) || sender.empty() ||
            !readString(message, "content", content) || content.empty() ||
            !readSignedInteger(message, "created_at", createdAt) ||
            createdAt < 0) {
            return false;
        }
        std::int64_t groupId = 0;
        if ((kind == "GROUP" &&
             (!readSignedInteger(message, "group_id", groupId) || groupId <= 0)) ||
            (kind != "GROUP" && message.find("group_id") != message.end())) {
            return false;
        }
        previous = id;
    }
    hasMore = more->get<bool>();
    return next == (messages->empty() ? afterMessageId : previous) &&
        (!hasMore || (!messages->empty() && next > afterMessageId));
}

} // namespace

void ConsoleOutput::line(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    std::cout << text << std::endl;
}

void ConsoleOutput::error(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    std::cerr << text << std::endl;
}

void ConsoleOutput::prompt(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    activePrompt = text;
    std::cout << text << std::flush;
}

void ConsoleOutput::clearPrompt() {
    std::lock_guard<std::mutex> lock(mutex);
    activePrompt.clear();
}

void ConsoleOutput::event(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    std::cout << '\n' << text << std::endl;
    if (!activePrompt.empty()) {
        std::cout << activePrompt << std::flush;
    }
}

TerminalUi::TerminalUi(
    ClientConnection& newConnection,
    ConsoleInput& newInput,
    ConsoleOutput& newOutput)
    : connection(newConnection),
      input(newInput),
      output(newOutput),
      exitRequested(false) {}

void TerminalUi::handleFrame(ClientFrameKind kind, const json& frame) {
    if (kind == ClientFrameKind::Message) {
        std::int64_t messageId = 0;
        std::int64_t groupId = 0;
        std::string messageKind;
        std::string sender;
        std::string content;
        if (!readSignedInteger(frame, "message_id", messageId) ||
            !readString(frame, "kind", messageKind) ||
            !readString(frame, "sender", sender) ||
            !readString(frame, "content", content)) {
            output.event("[ERROR] Invalid MESSAGE display data");
            connection.requestStop("Invalid MESSAGE display data");
            return;
        }
        std::string label = "[MESSAGE #" + std::to_string(messageId) + " " +
            safeDisplayText(messageKind) + "] " + safeDisplayText(sender);
        if (messageKind == "GROUP" &&
            readSignedInteger(frame, "group_id", groupId)) {
            label += " in group " + std::to_string(groupId);
        }
        output.event(label + ": " + safeDisplayText(content));
        return;
    }

    bool associationError = false;
    {
        std::lock_guard<std::mutex> lock(responseMutex);
        if (expectedOperation.empty() || pendingResponse) {
            associationError = true;
        } else {
            const auto operation = frame.find("operation");
            if (operation != frame.end()) {
                associationError = !operation->is_string() ||
                    operation->get<std::string>() != expectedOperation;
            } else {
                std::string status;
                std::string message;
                associationError =
                    !readString(frame, "status", status) || status != "FAIL" ||
                    !readString(frame, "message", message);
            }
            if (!associationError) {
                pendingResponse = frame;
            }
        }
    }
    if (associationError) {
        output.event("[ERROR] Server response could not be associated safely");
        connection.requestStop("Server response association failed");
        return;
    }
    input.notifyWork();
}

bool TerminalUi::readLine(const std::string& prompt, std::string& line) {
    while (!exitRequested && !connection.isStopping()) {
        if (!connection.flushAutomaticRequests()) {
            return false;
        }
        output.prompt(prompt);
        const ConsoleReadResult result = input.readLine(line);
        output.clearPrompt();
        if (result == ConsoleReadResult::WorkAvailable) {
            continue;
        }
        if (result == ConsoleReadResult::Line) {
            return true;
        }
        if (result == ConsoleReadResult::Interrupted) {
            return false;
        }
        exitRequested = true;
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
    return false;
}

bool TerminalUi::readChoice(
    const std::string& menu,
    std::size_t maximum,
    bool allowBack,
    std::size_t& choice) {
    while (!exitRequested && !connection.isStopping()) {
        output.line(menu);
        std::string line;
        if (!readLine("Select: ", line)) {
            return false;
        }
        if (parseUnsignedChoice(line, choice) && choice <= maximum &&
            (allowBack || choice != 0)) {
            return true;
        }
        output.error("Enter one of the displayed numbers.");
    }
    return false;
}

bool TerminalUi::requestFits(const json& request) {
    try {
        if (request.dump().size() > ClientConnection::MaxOutboundJsonBytes) {
            output.error("[ERROR] Request exceeds the 65,536-byte limit");
            return false;
        }
    } catch (const json::exception&) {
        output.error("[ERROR] Text is not valid UTF-8");
        return false;
    }
    return true;
}

bool TerminalUi::performRequest(
    const std::string& operation,
    const json& request,
    json& response) {
    if (!requestFits(request)) {
        return false;
    }
    bool stateInconsistent = false;
    {
        std::lock_guard<std::mutex> lock(responseMutex);
        if (!expectedOperation.empty() || pendingResponse) {
            stateInconsistent = true;
        } else {
            expectedOperation = operation;
            pendingResponse.reset();
        }
    }
    if (stateInconsistent) {
        output.error("[ERROR] Another operation is already outstanding");
        connection.requestStop("Foreground operation state is inconsistent");
        return false;
    }

    const ClientSendResult result = connection.sendJson(request);
    if (result != ClientSendResult::Written) {
        clearOutstandingResponse();
        if (result == ClientSendResult::TooLarge) {
            output.error("[ERROR] Request exceeds the 65,536-byte limit");
        } else {
            output.error("[ERROR] Request write failed");
        }
        return false;
    }
    return waitForResponse(response);
}

bool TerminalUi::waitForResponse(json& response) {
    while (!exitRequested && !connection.isStopping()) {
        if (!connection.flushAutomaticRequests()) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(responseMutex);
            if (pendingResponse) {
                response = std::move(*pendingResponse);
                pendingResponse.reset();
                expectedOperation.clear();
                return true;
            }
        }

        output.line("Waiting for server response.\n1. Exit client");
        output.prompt("Select: ");
        std::string line;
        const ConsoleReadResult result = input.readLine(line);
        output.clearPrompt();
        if (result == ConsoleReadResult::WorkAvailable) {
            continue;
        }
        if (result == ConsoleReadResult::Line) {
            std::size_t choice = 0;
            if (parseUnsignedChoice(line, choice) && choice == 1) {
                exitRequested = true;
                return false;
            }
            output.error(
                "Please wait for the server response; 1 exits.");
            continue;
        }
        if (result == ConsoleReadResult::Interrupted) {
            return false;
        }
        exitRequested = true;
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
    return false;
}

void TerminalUi::clearOutstandingResponse() {
    std::lock_guard<std::mutex> lock(responseMutex);
    expectedOperation.clear();
    pendingResponse.reset();
}

void TerminalUi::stopForInvalidResponse() {
    output.error("[ERROR] Server returned an invalid operation response");
    connection.requestStop("Invalid operation response");
}

bool TerminalUi::showFailure(const json& response) {
    std::string status;
    if (!readString(response, "status", status)) {
        stopForInvalidResponse();
        return true;
    }
    if (status != "FAIL") {
        return false;
    }
    std::string message;
    if (!readString(response, "message", message)) {
        stopForInvalidResponse();
        return true;
    }
    std::string label = "Server [FAIL]";
    std::string detail;
    if (readString(response, "code", detail) ||
        readString(response, "outcome", detail)) {
        label += " (" + safeDisplayText(detail) + ")";
    }
    output.error(label + ": " + safeDisplayText(message));
    return true;
}

void TerminalUi::run() {
    mainMenu();
}

void TerminalUi::mainMenu() {
    const std::string menu =
        "=== Protected Chat ===\n"
        "1. Messages\n"
        "2. Groups\n"
        "3. Contacts and Search\n"
        "4. History\n"
        "5. Exit";
    while (!exitRequested && !connection.isStopping()) {
        std::size_t choice = 0;
        if (!readChoice(menu, 5, false, choice)) {
            return;
        }
        if (choice == 1) {
            messagesMenu();
        } else if (choice == 2) {
            groupsMenu();
        } else if (choice == 3) {
            contactsMenu();
        } else if (choice == 4) {
            history();
        } else {
            exitRequested = true;
        }
    }
}

void TerminalUi::messagesMenu() {
    const std::string menu =
        "=== Messages ===\n"
        "1. Private Chat from Contacts\n"
        "2. Private Chat by Username\n"
        "3. Group Chat from My Groups\n"
        "4. Broadcast\n"
        "0. Back";
    while (!exitRequested && !connection.isStopping()) {
        std::size_t choice = 0;
        if (!readChoice(menu, 4, true, choice) || choice == 0) {
            return;
        }
        if (choice == 1) {
            contactList();
        } else if (choice == 2) {
            while (!exitRequested && !connection.isStopping()) {
                std::string username;
                if (!readLine("Recipient username (empty line: Back): ", username)) {
                    return;
                }
                if (username.empty()) {
                    break;
                }
                if (username.find('\0') != std::string::npos ||
                    !requestFits({{"type", "PRIVATE"},
                                  {"receiver", username},
                                  {"content", "x"}})) {
                    output.error("[ERROR] Invalid recipient username");
                    continue;
                }
                composer("PRIVATE", username, std::nullopt);
                break;
            }
        } else if (choice == 3) {
            groupList();
        } else {
            composer("BROADCAST", std::string(), std::nullopt);
        }
    }
}

void TerminalUi::groupsMenu() {
    const std::string menu =
        "=== Groups ===\n"
        "1. My Groups\n"
        "2. Create Group\n"
        "3. Search and Join Groups\n"
        "4. Join Group by ID\n"
        "0. Back";
    while (!exitRequested && !connection.isStopping()) {
        std::size_t choice = 0;
        if (!readChoice(menu, 4, true, choice) || choice == 0) {
            return;
        }
        if (choice == 1) {
            groupList();
        } else if (choice == 2) {
            createGroupPrompt();
        } else if (choice == 3) {
            groupSearch();
        } else {
            joinGroupPrompt();
        }
    }
}

void TerminalUi::contactsMenu() {
    const std::string menu =
        "=== Contacts and Search ===\n"
        "1. My Contacts\n"
        "2. Add Contact by Username\n"
        "3. Search Users\n"
        "0. Back";
    while (!exitRequested && !connection.isStopping()) {
        std::size_t choice = 0;
        if (!readChoice(menu, 3, true, choice) || choice == 0) {
            return;
        }
        if (choice == 1) {
            contactList();
        } else if (choice == 2) {
            addContactPrompt();
        } else {
            userSearch();
        }
    }
}

void TerminalUi::contactList() {
    std::int64_t cursor = 0;
    json page;
    if (!performRequest(
            "CONTACT_LIST",
            {{"type", "CONTACT_LIST"},
             {"after_user_id", cursor},
             {"limit", PageLimit}},
            page) || showFailure(page)) {
        return;
    }
    while (!exitRequested && !connection.isStopping()) {
        std::int64_t next = 0;
        bool hasMore = false;
        if (!validContactPage(page, cursor, next, hasMore)) {
            stopForInvalidResponse();
            return;
        }
        const json& contacts = page["contacts"];
        std::ostringstream block;
        block << "=== My Contacts ===\n";
        for (std::size_t index = 0; index < contacts.size(); ++index) {
            block << index + 1 << ". "
                  << safeDisplayText(contacts[index]["username"].get<std::string>())
                  << " (User ID: "
                  << contacts[index]["user_id"].get<std::int64_t>() << ")\n";
        }
        if (contacts.empty()) {
            block << "No results.\n";
        }
        if (hasMore) {
            block << contacts.size() + 1 << ". Next Page\n";
        } else {
            block << "End of results.\n";
        }
        block << "0. Back";
        std::size_t choice = 0;
        const std::size_t maximum = contacts.size() + (hasMore ? 1 : 0);
        if (!readChoice(block.str(), maximum, true, choice) || choice == 0) {
            return;
        }
        if (choice <= contacts.size()) {
            composer(
                "PRIVATE",
                contacts[choice - 1]["username"].get<std::string>(),
                std::nullopt);
            continue;
        }
        json nextPage;
        if (!performRequest(
                "CONTACT_LIST",
                {{"type", "CONTACT_LIST"},
                 {"after_user_id", next},
                 {"limit", PageLimit}},
                nextPage)) {
            if (connection.isStopping() || exitRequested) {
                return;
            }
            continue;
        }
        if (showFailure(nextPage)) {
            continue;
        }
        std::int64_t checkedNext = 0;
        bool checkedMore = false;
        if (!validContactPage(nextPage, next, checkedNext, checkedMore)) {
            stopForInvalidResponse();
            return;
        }
        cursor = next;
        page = std::move(nextPage);
    }
}

void TerminalUi::groupList() {
    std::int64_t cursor = 0;
    json page;
    if (!performRequest(
            "GROUP_LIST",
            {{"type", "GROUP_LIST"},
             {"after_group_id", cursor},
             {"limit", PageLimit}},
            page) || showFailure(page)) {
        return;
    }
    while (!exitRequested && !connection.isStopping()) {
        std::int64_t next = 0;
        bool hasMore = false;
        if (!validGroupListPage(page, cursor, next, hasMore)) {
            stopForInvalidResponse();
            return;
        }
        const json& groups = page["groups"];
        std::ostringstream block;
        block << "=== My Groups ===\n";
        for (std::size_t index = 0; index < groups.size(); ++index) {
            block << index + 1 << ". "
                  << safeDisplayText(groups[index]["name"].get<std::string>())
                  << " (#" << groups[index]["id"].get<std::int64_t>() << ")\n";
        }
        if (groups.empty()) {
            block << "No results.\n";
        }
        if (hasMore) {
            block << groups.size() + 1 << ". Next Page\n";
        } else {
            block << "End of results.\n";
        }
        block << "0. Back";
        std::size_t choice = 0;
        const std::size_t maximum = groups.size() + (hasMore ? 1 : 0);
        if (!readChoice(block.str(), maximum, true, choice) || choice == 0) {
            return;
        }
        if (choice <= groups.size()) {
            const json& group = groups[choice - 1];
            composer(
                "GROUP",
                group["name"].get<std::string>(),
                group["id"].get<std::int64_t>());
            continue;
        }
        json nextPage;
        if (!performRequest(
                "GROUP_LIST",
                {{"type", "GROUP_LIST"},
                 {"after_group_id", next},
                 {"limit", PageLimit}},
                nextPage)) {
            if (connection.isStopping() || exitRequested) {
                return;
            }
            continue;
        }
        if (showFailure(nextPage)) {
            continue;
        }
        std::int64_t checkedNext = 0;
        bool checkedMore = false;
        if (!validGroupListPage(nextPage, next, checkedNext, checkedMore)) {
            stopForInvalidResponse();
            return;
        }
        cursor = next;
        page = std::move(nextPage);
    }
}

void TerminalUi::userSearch() {
    std::string query;
    while (!exitRequested && !connection.isStopping()) {
        if (!readLine("User search query (empty line: Back): ", query) ||
            query.empty()) {
            return;
        }
        if (query.size() > 128 || query.find('\0') != std::string::npos ||
            isAsciiWhitespaceOnly(query)) {
            output.error("[ERROR] Search query must contain 1 to 128 bytes");
            continue;
        }
        json check = {{"type", "USER_SEARCH"}, {"query", query},
                      {"after_user_id", 0}, {"limit", PageLimit}};
        if (requestFits(check)) {
            break;
        }
    }
    std::int64_t cursor = 0;
    json page;
    if (!performRequest(
            "USER_SEARCH",
            {{"type", "USER_SEARCH"}, {"query", query},
             {"after_user_id", cursor}, {"limit", PageLimit}},
            page) || showFailure(page)) {
        return;
    }
    while (!exitRequested && !connection.isStopping()) {
        std::int64_t next = 0;
        bool hasMore = false;
        if (!validUserSearchPage(page, query, cursor, next, hasMore)) {
            stopForInvalidResponse();
            return;
        }
        json& users = page["users"];
        std::ostringstream block;
        block << "=== User Search: " << safeDisplayText(query) << " ===\n";
        for (std::size_t index = 0; index < users.size(); ++index) {
            block << index + 1 << ". "
                  << safeDisplayText(users[index]["username"].get<std::string>())
                  << " (User ID: "
                  << users[index]["user_id"].get<std::int64_t>() << ", "
                  << (users[index]["is_contact"].get<bool>()
                      ? "contact" : "not a contact") << ")\n";
        }
        if (users.empty()) {
            block << "No results.\n";
        }
        if (hasMore) {
            block << users.size() + 1 << ". Next Page\n";
        } else {
            block << "End of results.\n";
        }
        block << "0. Back";
        std::size_t choice = 0;
        const std::size_t maximum = users.size() + (hasMore ? 1 : 0);
        if (!readChoice(block.str(), maximum, true, choice) || choice == 0) {
            return;
        }
        if (choice <= users.size()) {
            userActions(users[choice - 1]);
            continue;
        }
        json nextPage;
        if (!performRequest(
                "USER_SEARCH",
                {{"type", "USER_SEARCH"}, {"query", query},
                 {"after_user_id", next}, {"limit", PageLimit}},
                nextPage)) {
            if (connection.isStopping() || exitRequested) {
                return;
            }
            continue;
        }
        if (showFailure(nextPage)) {
            continue;
        }
        std::int64_t checkedNext = 0;
        bool checkedMore = false;
        if (!validUserSearchPage(
                nextPage, query, next, checkedNext, checkedMore)) {
            stopForInvalidResponse();
            return;
        }
        cursor = next;
        page = std::move(nextPage);
    }
}

void TerminalUi::groupSearch() {
    std::string query;
    while (!exitRequested && !connection.isStopping()) {
        if (!readLine("Group search query (empty line: Back): ", query) ||
            query.empty()) {
            return;
        }
        if (query.size() > 128 || query.find('\0') != std::string::npos ||
            isAsciiWhitespaceOnly(query)) {
            output.error("[ERROR] Search query must contain 1 to 128 bytes");
            continue;
        }
        json check = {{"type", "GROUP_SEARCH"}, {"query", query},
                      {"after_group_id", 0}, {"limit", PageLimit}};
        if (requestFits(check)) {
            break;
        }
    }
    std::int64_t cursor = 0;
    json page;
    if (!performRequest(
            "GROUP_SEARCH",
            {{"type", "GROUP_SEARCH"}, {"query", query},
             {"after_group_id", cursor}, {"limit", PageLimit}},
            page) || showFailure(page)) {
        return;
    }
    while (!exitRequested && !connection.isStopping()) {
        std::int64_t next = 0;
        bool hasMore = false;
        if (!validGroupSearchPage(page, query, cursor, next, hasMore)) {
            stopForInvalidResponse();
            return;
        }
        json& groups = page["groups"];
        std::ostringstream block;
        block << "=== Group Search: " << safeDisplayText(query) << " ===\n";
        for (std::size_t index = 0; index < groups.size(); ++index) {
            block << index + 1 << ". "
                  << safeDisplayText(groups[index]["name"].get<std::string>())
                  << " (#" << groups[index]["group_id"].get<std::int64_t>()
                  << ", " << (groups[index]["is_member"].get<bool>()
                      ? "member" : "not a member") << ")\n";
        }
        if (groups.empty()) {
            block << "No results.\n";
        }
        if (hasMore) {
            block << groups.size() + 1 << ". Next Page\n";
        } else {
            block << "End of results.\n";
        }
        block << "0. Back";
        std::size_t choice = 0;
        const std::size_t maximum = groups.size() + (hasMore ? 1 : 0);
        if (!readChoice(block.str(), maximum, true, choice) || choice == 0) {
            return;
        }
        if (choice <= groups.size()) {
            groupActions(groups[choice - 1]);
            continue;
        }
        json nextPage;
        if (!performRequest(
                "GROUP_SEARCH",
                {{"type", "GROUP_SEARCH"}, {"query", query},
                 {"after_group_id", next}, {"limit", PageLimit}},
                nextPage)) {
            if (connection.isStopping() || exitRequested) {
                return;
            }
            continue;
        }
        if (showFailure(nextPage)) {
            continue;
        }
        std::int64_t checkedNext = 0;
        bool checkedMore = false;
        if (!validGroupSearchPage(
                nextPage, query, next, checkedNext, checkedMore)) {
            stopForInvalidResponse();
            return;
        }
        cursor = next;
        page = std::move(nextPage);
    }
}

void TerminalUi::history() {
    std::int64_t cursor = 0;
    std::optional<std::int64_t> watermark;
    json request = {
        {"type", "HISTORY"},
        {"after_message_id", cursor},
        {"limit", PageLimit}
    };
    json page;
    if (!performRequest("HISTORY", request, page) || showFailure(page)) {
        return;
    }
    while (!exitRequested && !connection.isStopping()) {
        std::int64_t returnedWatermark = 0;
        std::int64_t next = 0;
        bool hasMore = false;
        if (!validHistoryPage(
                page, cursor, watermark, returnedWatermark, next, hasMore)) {
            stopForInvalidResponse();
            return;
        }
        if (!watermark) {
            watermark = returnedWatermark;
        }
        const json& messages = page["messages"];
        std::ostringstream block;
        block << "=== History ===\n";
        if (messages.empty()) {
            block << "No history records.\n";
        }
        for (const json& message : messages) {
            const std::string kind = message["kind"].get<std::string>();
            block << "[HISTORY #"
                  << message["message_id"].get<std::int64_t>() << " "
                  << safeDisplayText(kind) << "] "
                  << safeDisplayText(message["sender"].get<std::string>());
            if (kind == "GROUP") {
                block << " in group "
                      << message["group_id"].get<std::int64_t>();
            }
            block << " at " << message["created_at"].get<std::int64_t>()
                  << ": "
                  << safeDisplayText(message["content"].get<std::string>())
                  << "\n";
        }
        if (hasMore) {
            block << "1. Next Page\n";
        } else {
            block << "End of history.\n";
        }
        block << "0. Back";
        std::size_t choice = 0;
        if (!readChoice(block.str(), hasMore ? 1 : 0, true, choice) ||
            choice == 0) {
            return;
        }
        json nextPage;
        if (!performRequest(
                "HISTORY",
                {{"type", "HISTORY"},
                 {"after_message_id", next},
                 {"through_message_id", *watermark},
                 {"limit", PageLimit}},
                nextPage)) {
            if (connection.isStopping() || exitRequested) {
                return;
            }
            continue;
        }
        if (showFailure(nextPage)) {
            continue;
        }
        std::int64_t checkedWatermark = 0;
        std::int64_t checkedNext = 0;
        bool checkedMore = false;
        if (!validHistoryPage(
                nextPage,
                next,
                watermark,
                checkedWatermark,
                checkedNext,
                checkedMore)) {
            stopForInvalidResponse();
            return;
        }
        cursor = next;
        page = std::move(nextPage);
    }
}

void TerminalUi::addContactPrompt() {
    while (!exitRequested && !connection.isStopping()) {
        std::string username;
        if (!readLine("Contact username (empty line: Back): ", username) ||
            username.empty()) {
            return;
        }
        if (username.find('\0') != std::string::npos) {
            output.error("[ERROR] Invalid contact username");
            continue;
        }
        json response;
        if (!performRequest(
                "CONTACT_ADD",
                {{"type", "CONTACT_ADD"}, {"username", username}},
                response)) {
            if (connection.isStopping() || exitRequested) {
                return;
            }
            continue;
        }
        if (showFailure(response)) {
            return;
        }
        std::string code;
        std::string returnedUsername;
        std::int64_t userId = 0;
        const auto contact = response.find("contact");
        if (!isSuccessFor(response, "CONTACT_ADD") ||
            !readString(response, "code", code) ||
            (code != "ADDED" && code != "ALREADY_CONTACT") ||
            contact == response.end() || !contact->is_object() ||
            !readSignedInteger(*contact, "user_id", userId) || userId <= 0 ||
            !readString(*contact, "username", returnedUsername) ||
            returnedUsername != username) {
            stopForInvalidResponse();
            return;
        }
        output.line(code == "ADDED"
            ? "Contact added: " + safeDisplayText(returnedUsername)
            : "Already in your contacts: " + safeDisplayText(returnedUsername));
        return;
    }
}

void TerminalUi::createGroupPrompt() {
    while (!exitRequested && !connection.isStopping()) {
        std::string name;
        if (!readLine("Group name (empty line: Back): ", name) || name.empty()) {
            return;
        }
        if (name.size() > 128 || isAsciiWhitespaceOnly(name)) {
            output.error("[ERROR] Group name must contain 1 to 128 bytes");
            continue;
        }
        json response;
        if (!performRequest(
                "GROUP_CREATE",
                {{"type", "GROUP_CREATE"}, {"name", name}},
                response)) {
            if (connection.isStopping() || exitRequested) {
                return;
            }
            continue;
        }
        if (showFailure(response)) {
            return;
        }
        std::string outcome;
        std::string returnedName;
        std::int64_t groupId = 0;
        const auto group = response.find("group");
        if (!isSuccessFor(response, "GROUP_CREATE") ||
            !readString(response, "outcome", outcome) || outcome != "CREATED" ||
            group == response.end() || !group->is_object() ||
            !readSignedInteger(*group, "id", groupId) || groupId <= 0 ||
            !readString(*group, "name", returnedName) || returnedName != name) {
            stopForInvalidResponse();
            return;
        }
        output.line("Group created: " + safeDisplayText(returnedName) +
                    " (#" + std::to_string(groupId) + ")");
        return;
    }
}

void TerminalUi::joinGroupPrompt() {
    while (!exitRequested && !connection.isStopping()) {
        std::string text;
        if (!readLine("Group ID (empty line: Back): ", text) || text.empty()) {
            return;
        }
        std::int64_t groupId = 0;
        if (!parsePositiveInt64(text, groupId)) {
            output.error("[ERROR] Enter a positive group ID");
            continue;
        }
        json response;
        if (!performRequest(
                "GROUP_JOIN",
                {{"type", "GROUP_JOIN"}, {"group_id", groupId}},
                response)) {
            if (connection.isStopping() || exitRequested) {
                return;
            }
            continue;
        }
        if (showFailure(response)) {
            return;
        }
        std::string outcome;
        std::int64_t returnedId = 0;
        if (!isSuccessFor(response, "GROUP_JOIN") ||
            !readString(response, "outcome", outcome) ||
            (outcome != "JOINED" && outcome != "ALREADY_MEMBER") ||
            !readSignedInteger(response, "group_id", returnedId) ||
            returnedId != groupId) {
            stopForInvalidResponse();
            return;
        }
        output.line(outcome == "JOINED"
            ? "Group joined: #" + std::to_string(groupId)
            : "Already a member of group #" + std::to_string(groupId));
        return;
    }
}

void TerminalUi::userActions(json& user) {
    while (!exitRequested && !connection.isStopping()) {
        const std::string username = user["username"].get<std::string>();
        const std::int64_t userId = user["user_id"].get<std::int64_t>();
        const bool isContact = user["is_contact"].get<bool>();
        std::ostringstream menu;
        menu << "=== " << safeDisplayText(username) << " ===\n"
             << "User ID: " << userId << "\n"
             << "1. Start Private Chat\n";
        if (isContact) {
            menu << "Already in your contacts\n";
        } else {
            menu << "2. Add to Contacts\n";
        }
        menu << "0. Back";
        std::size_t choice = 0;
        if (!readChoice(menu.str(), isContact ? 1 : 2, true, choice) ||
            choice == 0) {
            return;
        }
        if (choice == 1) {
            composer("PRIVATE", username, std::nullopt);
            continue;
        }
        json response;
        if (!performRequest(
                "CONTACT_ADD",
                {{"type", "CONTACT_ADD"}, {"username", username}},
                response)) {
            if (connection.isStopping() || exitRequested) {
                return;
            }
            continue;
        }
        if (showFailure(response)) {
            continue;
        }
        std::string code;
        std::string returnedUsername;
        std::int64_t returnedId = 0;
        const auto contact = response.find("contact");
        if (!isSuccessFor(response, "CONTACT_ADD") ||
            !readString(response, "code", code) ||
            (code != "ADDED" && code != "ALREADY_CONTACT") ||
            contact == response.end() || !contact->is_object() ||
            !readSignedInteger(*contact, "user_id", returnedId) ||
            returnedId != userId ||
            !readString(*contact, "username", returnedUsername) ||
            returnedUsername != username) {
            stopForInvalidResponse();
            return;
        }
        user["is_contact"] = true;
        output.line(code == "ADDED"
            ? "Contact added: " + safeDisplayText(username)
            : "Already in your contacts: " + safeDisplayText(username));
    }
}

void TerminalUi::groupActions(json& group) {
    while (!exitRequested && !connection.isStopping()) {
        const std::string name = group["name"].get<std::string>();
        const std::int64_t groupId = group["group_id"].get<std::int64_t>();
        const bool isMember = group["is_member"].get<bool>();
        std::ostringstream menu;
        menu << "=== " << safeDisplayText(name) << " ===\n"
             << "Group ID: " << groupId << "\n"
             << "1. " << (isMember ? "Start Group Chat" : "Join Group")
             << "\n0. Back";
        std::size_t choice = 0;
        if (!readChoice(menu.str(), 1, true, choice) || choice == 0) {
            return;
        }
        if (isMember) {
            composer("GROUP", name, groupId);
            continue;
        }
        json response;
        if (!performRequest(
                "GROUP_JOIN",
                {{"type", "GROUP_JOIN"}, {"group_id", groupId}},
                response)) {
            if (connection.isStopping() || exitRequested) {
                return;
            }
            continue;
        }
        if (showFailure(response)) {
            continue;
        }
        std::string outcome;
        std::int64_t returnedId = 0;
        if (!isSuccessFor(response, "GROUP_JOIN") ||
            !readString(response, "outcome", outcome) ||
            (outcome != "JOINED" && outcome != "ALREADY_MEMBER") ||
            !readSignedInteger(response, "group_id", returnedId) ||
            returnedId != groupId) {
            stopForInvalidResponse();
            return;
        }
        group["is_member"] = true;
        output.line(outcome == "JOINED"
            ? "Group joined: #" + std::to_string(groupId)
            : "Already a member of group #" + std::to_string(groupId));
    }
}

void TerminalUi::composer(
    const std::string& kind,
    const std::string& targetName,
    const std::optional<std::int64_t>& groupId) {
    std::string menu;
    if (kind == "PRIVATE") {
        menu = "=== Chat with " + safeDisplayText(targetName) + " ===\n";
    } else if (kind == "GROUP") {
        menu = "=== Group: " + safeDisplayText(targetName) + " (#" +
            std::to_string(*groupId) + ") ===\n";
    } else {
        menu = "=== Broadcast ===\n"
               "To all other registered users, including offline users.\n";
    }
    menu += "1. Send Message\n0. Back";

    while (!exitRequested && !connection.isStopping()) {
        std::size_t choice = 0;
        if (!readChoice(menu, 1, true, choice) || choice == 0) {
            return;
        }
        while (!exitRequested && !connection.isStopping()) {
            std::string content;
            if (!readLine("Message (empty line: Back): ", content)) {
                return;
            }
            if (content.empty()) {
                break;
            }
            if (content.find('\0') != std::string::npos) {
                output.error("[ERROR] Message contains an invalid NUL byte");
                continue;
            }
            json request = {{"type", kind}, {"content", content}};
            if (kind == "PRIVATE") {
                request["receiver"] = targetName;
            } else if (kind == "GROUP") {
                request["group_id"] = *groupId;
            }
            if (!requestFits(request)) {
                continue;
            }
            json response;
            if (!performRequest(kind, request, response)) {
                if (connection.isStopping() || exitRequested) {
                    return;
                }
                continue;
            }
            if (showFailure(response)) {
                break;
            }
            std::string code;
            std::string returnedKind;
            std::int64_t messageId = 0;
            std::int64_t recipientCount = 0;
            if (!isSuccessFor(response, kind) ||
                !readString(response, "code", code) || code != "ACCEPTED" ||
                !readSignedInteger(response, "message_id", messageId) ||
                messageId <= 0 || !readString(response, "kind", returnedKind) ||
                returnedKind != kind ||
                !readSignedInteger(
                    response, "recipient_count", recipientCount) ||
                recipientCount < 0) {
                stopForInvalidResponse();
                return;
            }
            output.line("Accepted message #" + std::to_string(messageId));
            break;
        }
    }
}
