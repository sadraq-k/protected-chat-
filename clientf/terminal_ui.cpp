#include "terminal_ui.h"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

using json = nlohmann::json;

namespace {

bool asciiWhitespace(unsigned char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
        character == '\n' || character == '\f' || character == '\v';
}

std::string trimAscii(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() && asciiWhitespace(text[first])) ++first;
    std::size_t last = text.size();
    while (last > first && asciiWhitespace(text[last - 1])) --last;
    return text.substr(first, last - first);
}

bool whitespaceOnly(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return asciiWhitespace(character);
    });
}

bool parseChoice(const std::string& input, std::size_t& value) {
    const std::string text = trimAscii(input);
    if (text.empty()) return false;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
        parsed > std::numeric_limits<std::size_t>::max()) return false;
    value = static_cast<std::size_t>(parsed);
    return true;
}

bool parsePositive(const std::string& input, std::int64_t& value) {
    const std::string text = trimAscii(input);
    if (text.empty()) return false;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
        parsed == 0 || parsed > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) return false;
    value = static_cast<std::int64_t>(parsed);
    return true;
}

bool readInteger(const json& value, const char* field, std::int64_t& result) {
    const auto found = value.find(field);
    if (found == value.end()) return false;
    if (found->is_number_unsigned()) {
        const auto parsed = found->get<std::uint64_t>();
        if (parsed > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) return false;
        result = static_cast<std::int64_t>(parsed);
        return true;
    }
    if (!found->is_number_integer()) return false;
    result = found->get<std::int64_t>();
    return true;
}

bool readString(const json& value, const char* field, std::string& result) {
    const auto found = value.find(field);
    if (found == value.end() || !found->is_string()) return false;
    result = found->get<std::string>();
    return true;
}

bool successFor(const json& value, const std::string& operation) {
    std::string status;
    std::string actual;
    std::string message;
    return value.is_object() && readString(value, "status", status) &&
        status == "SUCCESS" && readString(value, "operation", actual) &&
        actual == operation && readString(value, "message", message);
}

bool singleLineText(const std::string& text) {
    for (unsigned char byte : text)
        if (byte < 0x20u || byte == 0x7fu) return false;
    return true;
}

bool validGroupListResult(
    const json& response,
    std::int64_t afterGroupId,
    std::size_t limit,
    std::int64_t& nextGroupId) {
    if (!successFor(response, "GROUP_LIST")) return false;
    const auto groups = response.find("groups");
    const auto more = response.find("has_more");
    if (groups == response.end() || !groups->is_array() ||
        groups->size() > limit || more == response.end() ||
        !more->is_boolean() ||
        !readInteger(response, "next_after_group_id", nextGroupId) ||
        nextGroupId < afterGroupId) return false;
    std::int64_t previous = afterGroupId;
    for (const json& group : *groups) {
        std::int64_t id = 0;
        std::string name;
        if (!group.is_object() || !readInteger(group, "id", id) ||
            id <= previous || !readString(group, "name", name)) return false;
        previous = id;
    }
    return nextGroupId == (groups->empty() ? afterGroupId : previous) &&
        (!more->get<bool>() || (!groups->empty() && nextGroupId > afterGroupId));
}

std::size_t previousBoundary(const std::string& text, std::size_t cursor) {
    if (cursor == 0) return 0;
    --cursor;
    while (cursor > 0 &&
           (static_cast<unsigned char>(text[cursor]) & 0xc0u) == 0x80u)
        --cursor;
    return cursor;
}

std::size_t nextBoundary(const std::string& text, std::size_t cursor) {
    if (cursor >= text.size()) return text.size();
    ++cursor;
    while (cursor < text.size() &&
           (static_cast<unsigned char>(text[cursor]) & 0xc0u) == 0x80u)
        ++cursor;
    return cursor;
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

bool InputField::insert(const std::string& text) {
    if (text.size() > MaxBytes - bytes.size()) return false;
    bytes.insert(byteCursor, text);
    byteCursor += text.size();
    ++editRevision;
    return true;
}
bool InputField::eraseBefore() {
    if (byteCursor == 0) return false;
    const std::size_t previous = previousBoundary(bytes, byteCursor);
    bytes.erase(previous, byteCursor - previous);
    byteCursor = previous;
    ++editRevision;
    return true;
}
bool InputField::eraseAt() {
    if (byteCursor >= bytes.size()) return false;
    bytes.erase(byteCursor, nextBoundary(bytes, byteCursor) - byteCursor);
    ++editRevision;
    return true;
}
bool InputField::moveLeft() {
    const std::size_t previous = previousBoundary(bytes, byteCursor);
    if (previous == byteCursor) return false;
    byteCursor = previous;
    return true;
}
bool InputField::moveRight() {
    const std::size_t next = nextBoundary(bytes, byteCursor);
    if (next == byteCursor) return false;
    byteCursor = next;
    return true;
}
void InputField::home() noexcept { byteCursor = 0; }
void InputField::end() noexcept { byteCursor = bytes.size(); }
void InputField::clear() {
    if (!bytes.empty()) {
        bytes.clear();
        byteCursor = 0;
        ++editRevision;
    }
}
bool InputField::replace(const std::string& text) {
    if (text.size() > MaxBytes) return false;
    bytes = text;
    byteCursor = bytes.size();
    ++editRevision;
    return true;
}
const std::string& InputField::text() const noexcept { return bytes; }
std::size_t InputField::cursor() const noexcept { return byteCursor; }
std::uint64_t InputField::revision() const noexcept { return editRevision; }

ScreenState::ScreenState(ScreenKind newKind) : kind(newKind) {}

PendingOperation::PendingOperation(
    OperationPurpose purpose, std::string operation, ScreenKind origin,
    std::int64_t cursor, std::size_t limit,
    std::optional<std::int64_t> targetGroupId)
    : operationPurpose(purpose), operationName(std::move(operation)),
      originScreen(origin), requestCursor(cursor), requestLimit(limit),
      lookupGroupId(targetGroupId) {}
OperationPurpose PendingOperation::purpose() const noexcept {
    return operationPurpose;
}
const std::string& PendingOperation::operation() const noexcept {
    return operationName;
}
ScreenKind PendingOperation::origin() const noexcept { return originScreen; }
std::int64_t PendingOperation::cursor() const noexcept { return requestCursor; }
std::size_t PendingOperation::limit() const noexcept { return requestLimit; }
const std::optional<std::int64_t>& PendingOperation::targetGroupId() const noexcept {
    return lookupGroupId;
}

TerminalUi::TerminalUi(
    ClientConnection& newConnection, ConsoleInput& newInput,
    ConsoleOutput& newOutput)
    : connection(newConnection), input(newInput), output(newOutput) {
    screens.emplace_back(ScreenKind::Main);
}

void TerminalUi::setAuthenticatedUsername(const std::string& username) {
    authenticatedUsername = username;
}

void TerminalUi::handleFrame(ClientFrameKind kind, const json& frame) {
    if (kind == ClientFrameKind::Message) {
        try {
            const BufferInsertResult result = messageBuffer.receiveValidated(frame);
            if (result == BufferInsertResult::CapacityExceeded) {
                connection.requestStop("Session message capacity exceeded");
                input.notifyWork();
                return;
            }
            if (result == BufferInsertResult::Conflict) {
                connection.requestStop("Conflicting durable message received");
                input.notifyWork();
                return;
            }
            if (result == BufferInsertResult::Stored) input.notifyWork();
        } catch (...) {
            connection.requestStop("Could not retain incoming message");
            input.notifyWork();
        }
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
                associationError = !readString(frame, "status", status) ||
                    status != "FAIL" || !readString(frame, "message", message);
            }
            if (!associationError) pendingResponse = frame;
        }
    }
    if (associationError) {
        connection.requestStop("Server response association failed");
        input.notifyWork();
        return;
    }
    input.notifyWork();
}

void TerminalUi::handleDiagnostic(const std::string& message, bool error) {
    {
        std::lock_guard<std::mutex> lock(responseMutex);
        diagnostic = message.substr(0, MaxDiagnosticBytes);
        diagnosticIsError = error;
        ++diagnosticCount;
    }
    input.notifyWork();
}

ScreenState& TerminalUi::current() { return screens.back(); }
const ScreenState& TerminalUi::current() const { return screens.back(); }

bool TerminalUi::push(ScreenState state) {
    if (screens.size() >= MaxScreenDepth) {
        connection.requestStop("Terminal navigation depth exceeded");
        return false;
    }
    screens.push_back(std::move(state));
    dirty = true;
    return true;
}

void TerminalUi::back() {
    if (current().kind == ScreenKind::Main) {
        setStatus("Use 6 or Ctrl+Q to exit.");
        return;
    }
    if (current().kind == ScreenKind::UserSearchResults) {
        current().kind = ScreenKind::UserSearchInput;
        current().input.replace(current().query);
        current().hasPage = false;
    } else if (current().kind == ScreenKind::GroupSearchResults) {
        current().kind = ScreenKind::GroupSearchInput;
        current().input.replace(current().query);
        current().hasPage = false;
    } else if (screens.size() > 1) {
        screens.pop_back();
    }
    dirty = true;
}

void TerminalUi::openNotifications() {
    if (current().kind == ScreenKind::Notifications) {
        back();
        return;
    }
    if (screens.size() >= 2 &&
        screens[screens.size() - 2].kind == ScreenKind::Notifications) {
        back();
        return;
    }
    push(ScreenState(ScreenKind::Notifications));
}

TerminalUi::ConversationUiState& TerminalUi::conversationState(
    const ConversationKey& key) {
    return conversationStates[key];
}

void TerminalUi::openConversation(const ConversationKey& key) {
    const BufferInsertResult result = messageBuffer.ensureConversation(key);
    if (result == BufferInsertResult::CapacityExceeded) {
        setStatus("Session conversation capacity reached");
        return;
    }
    ScreenState state(ScreenKind::Conversation);
    state.conversation = key;
    push(std::move(state));
    conversationState(key).followLatest = true;
}

InputField* TerminalUi::activeInput() {
    if (current().kind == ScreenKind::Conversation && current().conversation)
        return &conversationState(*current().conversation).draft;
    return &current().input;
}
const InputField* TerminalUi::activeInput() const {
    if (current().kind == ScreenKind::Conversation && current().conversation) {
        const auto found = conversationStates.find(*current().conversation);
        return found == conversationStates.end() ? nullptr : &found->second.draft;
    }
    return &current().input;
}
bool TerminalUi::activeInputIsMultiline() const {
    return current().kind == ScreenKind::Conversation;
}

void TerminalUi::setStatus(std::string message) {
    statusMessage = message.substr(0, MaxDiagnosticBytes);
    dirty = true;
}

void TerminalUi::requestExit(std::string reason) {
    exitReason = std::move(reason);
    exitRequested = true;
    dirty = true;
}

bool TerminalUi::terminalTooSmall(const TerminalSize& size) const noexcept {
    return size.columns() < 40 || size.rows() < 12;
}

void TerminalUi::run(TerminalScreen& terminal) {
    std::uint64_t renderedVersion = std::numeric_limits<std::uint64_t>::max();
    while (!exitRequested) {
        consumeResponse();
        if (connection.isStopping()) break;
        const std::optional<ConversationKey> active =
            current().kind == ScreenKind::Conversation ? current().conversation
                                                       : std::nullopt;
        BufferSnapshot snapshot = messageBuffer.snapshot(active);
        const TerminalSize terminalSize = terminal.size();
        if (terminal.sizeQueryFailed() && !terminalSizeWarningActive) {
            setStatus("Could not read terminal size; using the last known size");
            terminalSizeWarningActive = true;
        } else if (!terminal.sizeQueryFailed()) {
            terminalSizeWarningActive = false;
        }
        const bool tooSmall = terminalTooSmall(terminalSize);
        if (dirty || snapshot.version() != renderedVersion ||
            tooSmall != lastScreenTooSmall) {
            terminal.present(renderCurrentScreen(terminalSize, snapshot));
            renderedVersion = snapshot.version();
            dirty = false;
            lastScreenTooSmall = tooSmall;
            if (!tooSmall && current().kind == ScreenKind::Conversation &&
                current().conversation &&
                conversationState(*current().conversation).followLatest &&
                messageBuffer.markViewed(
                    *current().conversation,
                    snapshot.maximumArrivalSequence())) {
                snapshot = messageBuffer.snapshot(current().conversation);
                terminal.present(renderCurrentScreen(terminalSize, snapshot));
                renderedVersion = snapshot.version();
            }
        }
        if (!connection.flushAutomaticRequests(1)) break;
        scheduleNameWork();
        processEvent(input.readEvent());
    }
    if (pendingOperation && submittedKey) {
        uncertainAcceptance = true;
        messageBuffer.cancelOutgoing();
    }
    if (exitReason.empty()) exitReason = connection.stopReason();
}

void TerminalUi::processEvent(const ConsoleEvent& event) {
    if (event.key() == ConsoleKey::WorkAvailable ||
        event.key() == ConsoleKey::Redraw) {
        dirty = true;
        return;
    }
    if (event.key() == ConsoleKey::Interrupted) {
        const std::string reason = connection.stopReason();
        requestExit(reason.empty() ? "Client interrupted" : reason);
        return;
    }
    if (event.key() == ConsoleKey::EndOfFile ||
        event.key() == ConsoleKey::Exit) {
        requestExit(event.key() == ConsoleKey::EndOfFile
            ? "Standard input closed" : "User requested exit");
        return;
    }
    if (lastScreenTooSmall) return;
    if (event.key() == ConsoleKey::Notifications) {
        openNotifications();
        return;
    }
    if (event.key() == ConsoleKey::Escape) {
        processEscape();
        return;
    }
    if (event.key() == ConsoleKey::Enter) {
        processEnter();
        return;
    }
    if (event.key() == ConsoleKey::Text || event.key() == ConsoleKey::Paste) {
        processText(event.text(), event.key() == ConsoleKey::Paste);
        return;
    }
    if (event.key() == ConsoleKey::InputTooLarge) {
        setStatus("Pasted input exceeds 65,536 bytes");
        return;
    }
    if (event.key() == ConsoleKey::PageUp ||
        event.key() == ConsoleKey::PageDown ||
        event.key() == ConsoleKey::FollowLatest ||
        event.key() == ConsoleKey::RestoreFailed ||
        event.key() == ConsoleKey::Backspace ||
        event.key() == ConsoleKey::Delete ||
        event.key() == ConsoleKey::Left || event.key() == ConsoleKey::Right ||
        event.key() == ConsoleKey::Home || event.key() == ConsoleKey::End ||
        event.key() == ConsoleKey::ClearInput) {
        processEditingKey(event.key());
        return;
    }
    if (event.key() == ConsoleKey::Unsupported) {
        requestExit("Full-screen input is unsupported");
    } else if (event.key() == ConsoleKey::Failure) {
        requestExit("Terminal input failed");
    } else {
        setStatus("Unsupported key");
    }
}

void TerminalUi::processText(const std::string& text, bool paste) {
    if (!protected_chat::terminal_detail::validUtf8(text) ||
        text.find('\0') != std::string::npos) {
        setStatus("Input is not valid UTF-8 or contains NUL");
        return;
    }
    if (paste && !activeInputIsMultiline() && !singleLineText(text)) {
        setStatus("Control characters are not allowed in this field");
        return;
    }
    InputField* field = activeInput();
    if (field == nullptr || !field->insert(text)) {
        setStatus("Input exceeds 65,536 bytes");
        return;
    }
    dirty = true;
}

void TerminalUi::processEditingKey(ConsoleKey key) {
    if (key == ConsoleKey::PageUp || key == ConsoleKey::PageDown) {
        std::size_t* scroll = &current().scroll;
        if (current().kind == ScreenKind::Conversation && current().conversation) {
            auto& state = conversationState(*current().conversation);
            scroll = &state.scroll;
            if (!state.followLatest && state.anchorMessageId) {
                state.anchorMoveRows = key == ConsoleKey::PageUp ? -10 : 10;
                dirty = true;
                return;
            }
        }
        if (key == ConsoleKey::PageUp)
            *scroll = *scroll > std::numeric_limits<std::size_t>::max() - 10
                ? std::numeric_limits<std::size_t>::max() : *scroll + 10;
        else *scroll = *scroll > 10 ? *scroll - 10 : 0;
        if (current().kind == ScreenKind::Conversation && current().conversation)
            conversationState(*current().conversation).followLatest =
                key == ConsoleKey::PageDown && *scroll == 0;
        dirty = true;
        return;
    }
    if (key == ConsoleKey::FollowLatest) {
        if (current().kind == ScreenKind::Conversation && current().conversation) {
            auto& state = conversationState(*current().conversation);
            state.scroll = 0;
            state.anchorMessageId.reset();
            state.anchorMoveRows = 0;
            state.followLatest = true;
            dirty = true;
        } else setStatus("Ctrl+End is available in conversations");
        return;
    }
    if (key == ConsoleKey::RestoreFailed) {
        if (current().kind == ScreenKind::Conversation && current().conversation) {
            auto& state = conversationState(*current().conversation);
            if (state.lastFailed.empty()) setStatus("No failed message to restore");
            else if (state.draft.replace(state.lastFailed)) {
                setStatus("Failed message restored; Enter retries manually");
            }
        } else setStatus("Ctrl+R is available in conversations");
        return;
    }
    InputField* field = activeInput();
    if (field == nullptr) return;
    if (key == ConsoleKey::Backspace) field->eraseBefore();
    else if (key == ConsoleKey::Delete) field->eraseAt();
    else if (key == ConsoleKey::Left) field->moveLeft();
    else if (key == ConsoleKey::Right) field->moveRight();
    else if (key == ConsoleKey::Home) field->home();
    else if (key == ConsoleKey::End) field->end();
    else if (key == ConsoleKey::ClearInput) field->clear();
    dirty = true;
}

void TerminalUi::processEscape() {
    if (pendingOperation &&
        pendingOperation->purpose() == OperationPurpose::Foreground &&
        current().kind != ScreenKind::Notifications) {
        setStatus("Waiting for the current server response");
        return;
    }
    back();
}

void TerminalUi::processEnter() {
    if (pendingOperation &&
        pendingOperation->purpose() == OperationPurpose::Foreground) {
        setStatus("Waiting for acceptance or server response");
        return;
    }
    if (current().kind == ScreenKind::Conversation) submitConversation();
    else if (current().kind == ScreenKind::PrivateTargetInput ||
             current().kind == ScreenKind::ContactAddInput ||
             current().kind == ScreenKind::GroupCreateInput ||
             current().kind == ScreenKind::GroupJoinInput ||
             current().kind == ScreenKind::UserSearchInput ||
             current().kind == ScreenKind::GroupSearchInput) submitForm();
    else {
        std::size_t choice = 0;
        if (!parseChoice(current().input.text(), choice)) {
            setStatus("Enter one of the displayed numbers");
            return;
        }
        if (current().kind == ScreenKind::ContactList ||
            current().kind == ScreenKind::GroupList ||
            current().kind == ScreenKind::UserSearchResults ||
            current().kind == ScreenKind::GroupSearchResults ||
            current().kind == ScreenKind::History ||
            current().kind == ScreenKind::Notifications)
            submitListSelection(choice);
        else submitMenu(choice);
    }
}

bool TerminalUi::submitMenu(std::size_t choice) {
    const ScreenKind kind = current().kind;
    if (choice == 0 && kind != ScreenKind::Main) {
        current().input.clear();
        back();
        return true;
    }
    current().input.clear();
    if (kind == ScreenKind::Main) {
        if (choice == 1) return push(ScreenState(ScreenKind::Messages));
        if (choice == 2) return push(ScreenState(ScreenKind::Groups));
        if (choice == 3) return push(ScreenState(ScreenKind::Contacts));
        if (choice == 4) {
            ScreenState state(ScreenKind::History);
            if (!push(std::move(state))) return false;
            return beginOperation(
                PendingOperation(OperationPurpose::Foreground, "HISTORY",
                                 ScreenKind::History, 0, PageLimit),
                {{"type", "HISTORY"}, {"after_message_id", 0},
                 {"limit", PageLimit}});
        }
        if (choice == 5) { openNotifications(); return true; }
        if (choice == 6) { requestExit("User requested exit"); return true; }
    } else if (kind == ScreenKind::Messages) {
        if (choice == 1) {
            if (!push(ScreenState(ScreenKind::ContactList))) return false;
            return beginOperation(
                PendingOperation(OperationPurpose::Foreground, "CONTACT_LIST",
                                 ScreenKind::ContactList, 0, PageLimit),
                {{"type", "CONTACT_LIST"}, {"after_user_id", 0},
                 {"limit", PageLimit}});
        }
        if (choice == 2) return push(ScreenState(ScreenKind::PrivateTargetInput));
        if (choice == 3) {
            if (!push(ScreenState(ScreenKind::GroupList))) return false;
            return beginOperation(
                PendingOperation(OperationPurpose::Foreground, "GROUP_LIST",
                                 ScreenKind::GroupList, 0, PageLimit),
                {{"type", "GROUP_LIST"}, {"after_group_id", 0},
                 {"limit", PageLimit}});
        }
        if (choice == 4) { openConversation(ConversationKey::broadcast()); return true; }
    } else if (kind == ScreenKind::Groups) {
        if (choice == 1) {
            if (!push(ScreenState(ScreenKind::GroupList))) return false;
            return beginOperation(
                PendingOperation(OperationPurpose::Foreground, "GROUP_LIST",
                                 ScreenKind::GroupList, 0, PageLimit),
                {{"type", "GROUP_LIST"}, {"after_group_id", 0},
                 {"limit", PageLimit}});
        }
        if (choice == 2) return push(ScreenState(ScreenKind::GroupCreateInput));
        if (choice == 3) return push(ScreenState(ScreenKind::GroupSearchInput));
        if (choice == 4) return push(ScreenState(ScreenKind::GroupJoinInput));
    } else if (kind == ScreenKind::Contacts) {
        if (choice == 1) {
            if (!push(ScreenState(ScreenKind::ContactList))) return false;
            return beginOperation(
                PendingOperation(OperationPurpose::Foreground, "CONTACT_LIST",
                                 ScreenKind::ContactList, 0, PageLimit),
                {{"type", "CONTACT_LIST"}, {"after_user_id", 0},
                 {"limit", PageLimit}});
        }
        if (choice == 2) return push(ScreenState(ScreenKind::ContactAddInput));
        if (choice == 3) return push(ScreenState(ScreenKind::UserSearchInput));
    } else if (kind == ScreenKind::UserActions) {
        if (choice == 1) {
            openConversation(ConversationKey::privateChat(
                current().selected["username"].get<std::string>()));
            return true;
        }
        if (choice == 2 && !current().selected["is_contact"].get<bool>()) {
            const std::string username = current().selected["username"];
            return beginOperation(
                PendingOperation(OperationPurpose::Foreground, "CONTACT_ADD",
                                 ScreenKind::UserActions, 0, 0),
                {{"type", "CONTACT_ADD"}, {"username", username}});
        }
    } else if (kind == ScreenKind::GroupActions) {
        const std::int64_t id = current().selected["group_id"];
        if (choice == 1 && current().selected["is_member"].get<bool>()) {
            cacheGroupName(id, current().selected["name"]);
            openConversation(ConversationKey::groupChat(id));
            return true;
        }
        if (choice == 1) {
            return beginOperation(
                PendingOperation(OperationPurpose::Foreground, "GROUP_JOIN",
                                 ScreenKind::GroupActions, 0, 0, id),
                {{"type", "GROUP_JOIN"}, {"group_id", id}});
        }
    }
    setStatus("Enter one of the displayed numbers");
    return false;
}

bool TerminalUi::submitForm() {
    ScreenState& state = current();
    const std::string text = state.input.text();
    if (text.empty()) {
        setStatus("This field cannot be empty; Esc goes back");
        return false;
    }
    if (!protected_chat::terminal_detail::validUtf8(text) ||
        text.find('\0') != std::string::npos) {
        setStatus("Input is not valid UTF-8 or contains NUL");
        return false;
    }
    if (state.kind == ScreenKind::PrivateTargetInput) {
        openConversation(ConversationKey::privateChat(text));
        return true;
    }
    if (state.kind == ScreenKind::ContactAddInput)
        return beginOperation(
            PendingOperation(OperationPurpose::Foreground, "CONTACT_ADD",
                             state.kind, 0, 0),
            {{"type", "CONTACT_ADD"}, {"username", text}});
    if (state.kind == ScreenKind::GroupCreateInput) {
        if (text.size() > 128 || whitespaceOnly(text)) {
            setStatus("Group name must contain 1 to 128 bytes");
            return false;
        }
        return beginOperation(
            PendingOperation(OperationPurpose::Foreground, "GROUP_CREATE",
                             state.kind, 0, 0),
            {{"type", "GROUP_CREATE"}, {"name", text}});
    }
    if (state.kind == ScreenKind::GroupJoinInput) {
        std::int64_t id = 0;
        if (!parsePositive(text, id)) {
            setStatus("Enter a positive group ID");
            return false;
        }
        return beginOperation(
            PendingOperation(OperationPurpose::Foreground, "GROUP_JOIN",
                             state.kind, 0, 0, id),
            {{"type", "GROUP_JOIN"}, {"group_id", id}});
    }
    if (text.size() > 128 || whitespaceOnly(text)) {
        setStatus("Search query must contain 1 to 128 bytes");
        return false;
    }
    state.query = text;
    const bool users = state.kind == ScreenKind::UserSearchInput;
    const std::string operation = users ? "USER_SEARCH" : "GROUP_SEARCH";
    return beginOperation(
        PendingOperation(OperationPurpose::Foreground, operation,
                         state.kind, 0, PageLimit),
        users ? json{{"type", operation}, {"query", text},
                     {"after_user_id", 0}, {"limit", PageLimit}}
              : json{{"type", operation}, {"query", text},
                     {"after_group_id", 0}, {"limit", PageLimit}});
}

bool TerminalUi::submitConversation() {
    if (!current().conversation) return false;
    const ConversationKey key = *current().conversation;
    auto& state = conversationState(key);
    const std::string content = state.draft.text();
    if (content.empty()) return false;
    if (!protected_chat::terminal_detail::validUtf8(content) ||
        content.find('\0') != std::string::npos) {
        setStatus("Message is not valid UTF-8 or contains NUL");
        return false;
    }
    json request = {{"content", content}};
    std::string operation;
    if (key.kind() == ConversationKind::Private) {
        operation = "PRIVATE";
        request["type"] = operation;
        request["receiver"] = key.username();
    } else if (key.kind() == ConversationKind::Group) {
        operation = "GROUP";
        request["type"] = operation;
        request["group_id"] = key.groupId();
    } else {
        operation = "BROADCAST";
        request["type"] = operation;
    }
    try {
        if (request.dump().size() > ClientConnection::MaxOutboundJsonBytes) {
            setStatus("Message request exceeds 65,536 bytes");
            return false;
        }
    } catch (...) {
        setStatus("Message is not valid UTF-8");
        return false;
    }
    const BufferInsertResult reserved =
        messageBuffer.reserveOutgoing(key, authenticatedUsername, content);
    if (reserved == BufferInsertResult::CapacityExceeded) {
        setStatus("Session message capacity reached; message was not sent");
        return false;
    }
    if (reserved != BufferInsertResult::Stored) {
        connection.requestStop("Outgoing message reservation conflict");
        return false;
    }
    submittedKey = key;
    submittedText = content;
    submittedRevision = state.draft.revision();
    if (!beginOperation(
            PendingOperation(OperationPurpose::Foreground, operation,
                             ScreenKind::Conversation, 0, 0), request)) {
        messageBuffer.cancelOutgoing();
        submittedKey.reset();
        submittedText.clear();
        return false;
    }
    setStatus("Waiting for acceptance");
    return true;
}

bool TerminalUi::submitListSelection(std::size_t choice) {
    ScreenState& state = current();
    if (choice == 0) { state.input.clear(); back(); return true; }
    if (state.kind == ScreenKind::Notifications) {
        const BufferSnapshot snapshot = messageBuffer.snapshot(std::nullopt);
        for (const auto& summary : snapshot.summaries()) {
            if (summary.handle() == choice) {
                if (summary.unreadCount() == 0) {
                    setStatus("That notification has been viewed");
                    return false;
                }
                state.input.clear();
                openConversation(summary.key());
                return true;
            }
        }
        setStatus("Enter a currently unread notification number");
        return false;
    }
    const char* arrayName = nullptr;
    if (state.kind == ScreenKind::ContactList) arrayName = "contacts";
    else if (state.kind == ScreenKind::GroupList) arrayName = "groups";
    else if (state.kind == ScreenKind::UserSearchResults) arrayName = "users";
    else if (state.kind == ScreenKind::GroupSearchResults) arrayName = "groups";
    if (arrayName != nullptr) {
        const json& records = state.page[arrayName];
        if (choice >= 1 && choice <= records.size()) {
            state.input.clear();
            const json record = records[choice - 1];
            if (state.kind == ScreenKind::ContactList) {
                openConversation(ConversationKey::privateChat(record["username"]));
            } else if (state.kind == ScreenKind::GroupList) {
                const std::int64_t id = record["id"];
                cacheGroupName(id, record["name"]);
                openConversation(ConversationKey::groupChat(id));
            } else {
                ScreenState actions(state.kind == ScreenKind::UserSearchResults
                    ? ScreenKind::UserActions : ScreenKind::GroupActions);
                actions.selected = record;
                push(std::move(actions));
            }
            return true;
        }
        const bool hasMore = state.page.value("has_more", false);
        if (hasMore && choice == records.size() + 1) {
            const std::string operation = state.kind == ScreenKind::ContactList
                ? "CONTACT_LIST" : state.kind == ScreenKind::GroupList
                ? "GROUP_LIST" : state.kind == ScreenKind::UserSearchResults
                ? "USER_SEARCH" : "GROUP_SEARCH";
            const char* cursorName = (state.kind == ScreenKind::ContactList ||
                                      state.kind == ScreenKind::UserSearchResults)
                ? "next_after_user_id" : "next_after_group_id";
            const std::int64_t next = state.page[cursorName];
            json request = {{"type", operation}, {"limit", PageLimit}};
            request[(state.kind == ScreenKind::ContactList ||
                     state.kind == ScreenKind::UserSearchResults)
                ? "after_user_id" : "after_group_id"] = next;
            if (state.kind == ScreenKind::UserSearchResults ||
                state.kind == ScreenKind::GroupSearchResults)
                request["query"] = state.query;
            state.input.clear();
            return beginOperation(
                PendingOperation(OperationPurpose::Foreground, operation,
                                 state.kind, next, PageLimit), request);
        }
    } else if (state.kind == ScreenKind::History) {
        if (state.page.value("has_more", false) && choice == 1) {
            const std::int64_t next = state.page["next_after_message_id"];
            json request = {{"type", "HISTORY"},
                            {"after_message_id", next}, {"limit", PageLimit},
                            {"through_message_id", *state.watermark}};
            state.input.clear();
            return beginOperation(
                PendingOperation(OperationPurpose::Foreground, "HISTORY",
                                 ScreenKind::History, next, PageLimit), request);
        }
    }
    setStatus("Enter one of the displayed numbers");
    return false;
}

bool TerminalUi::beginOperation(
    PendingOperation operation, const json& request) {
    if (pendingOperation) {
        setStatus("A request is finishing; press Enter again when ready");
        return false;
    }
    try {
        if (request.dump().size() > ClientConnection::MaxOutboundJsonBytes) {
            setStatus("Request exceeds 65,536 bytes");
            return false;
        }
    } catch (...) {
        setStatus("Request text is not valid UTF-8");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(responseMutex);
        if (!expectedOperation.empty() || pendingResponse) {
            connection.requestStop("Operation state is inconsistent");
            return false;
        }
        expectedOperation = operation.operation();
    }
    pendingOperation = std::move(operation);
    const ClientSendResult result = connection.sendJson(request);
    if (result == ClientSendResult::Written) {
        dirty = true;
        return true;
    }
    const bool outgoing = submittedKey.has_value();
    clearOperation();
    if (outgoing) {
        messageBuffer.cancelOutgoing();
        uncertainAcceptance = result == ClientSendResult::Failed;
        submittedKey.reset();
        submittedText.clear();
    }
    if (result == ClientSendResult::TooLarge)
        setStatus("Request exceeds 65,536 bytes");
    else
        requestExit("Request write failed; server acceptance is uncertain");
    return false;
}

bool TerminalUi::responseFailure(const json& response, std::string& detail) {
    std::string status;
    if (!readString(response, "status", status)) {
        stopForInvalidResponse();
        return true;
    }
    if (status != "FAIL") return false;
    std::string message;
    if (!readString(response, "message", message)) {
        stopForInvalidResponse();
        return true;
    }
    std::string code;
    if (!readString(response, "code", code)) readString(response, "outcome", code);
    detail = "Server [FAIL]" + (code.empty() ? std::string() : " (" + code + ")") +
        ": " + message;
    return true;
}

void TerminalUi::consumeResponse() {
    std::optional<json> response;
    {
        std::lock_guard<std::mutex> lock(responseMutex);
        if (pendingResponse) response = *pendingResponse;
    }
    if (!response) return;
    if (!pendingOperation) {
        connection.requestStop("Response has no operation context");
        return;
    }
    const PendingOperation operation = *pendingOperation;
    if (operation.purpose() == OperationPurpose::Foreground)
        applyForegroundResponse(operation, *response);
    else
        applyNameResponse(operation, *response);
    clearOperation();
    dirty = true;
}

void TerminalUi::clearOperation() {
    {
        std::lock_guard<std::mutex> lock(responseMutex);
        expectedOperation.clear();
        pendingResponse.reset();
    }
    pendingOperation.reset();
}

void TerminalUi::stopForInvalidResponse() {
    connection.requestStop("Invalid operation response");
    setStatus("Server returned an invalid operation response");
}

void TerminalUi::applyForegroundResponse(
    const PendingOperation& operation, const json& response) {
    std::string failure;
    if (responseFailure(response, failure)) {
        if (!failure.empty()) setStatus(failure);
        if (submittedKey) {
            auto& state = conversationState(*submittedKey);
            state.lastFailed = submittedText;
            messageBuffer.cancelOutgoing();
            submittedKey.reset();
            submittedText.clear();
        }
        return;
    }
    if (!successFor(response, operation.operation())) {
        stopForInvalidResponse();
        return;
    }
    if (operation.origin() == ScreenKind::Conversation && submittedKey) {
        std::string code;
        std::string kind;
        std::int64_t id = 0;
        std::int64_t recipients = 0;
        if (!readString(response, "code", code) || code != "ACCEPTED" ||
            !readString(response, "kind", kind) || kind != operation.operation() ||
            !readInteger(response, "message_id", id) || id <= 0 ||
            !readInteger(response, "recipient_count", recipients) || recipients < 0 ||
            messageBuffer.commitOutgoing(id) != BufferInsertResult::Stored) {
            stopForInvalidResponse();
            return;
        }
        auto& state = conversationState(*submittedKey);
        if (state.draft.revision() == submittedRevision) {
            state.draft.clear();
            setStatus("Accepted #" + std::to_string(id));
        } else {
            setStatus("Accepted #" + std::to_string(id) + "; edited draft retained");
        }
        submittedKey.reset();
        submittedText.clear();
        return;
    }

    std::size_t stateIndex = screens.size() - 1;
    if (current().kind == ScreenKind::Notifications && screens.size() >= 2)
        stateIndex = screens.size() - 2;
    if (screens[stateIndex].kind != operation.origin()) {
        stopForInvalidResponse();
        return;
    }
    ScreenState& state = screens[stateIndex];
    const std::string& name = operation.operation();
    if (name == "CONTACT_LIST" || name == "USER_SEARCH" ||
        name == "GROUP_LIST" || name == "GROUP_SEARCH" || name == "HISTORY") {
        const char* arrayName = name == "CONTACT_LIST" ? "contacts" :
            name == "USER_SEARCH" ? "users" :
            (name == "GROUP_LIST" || name == "GROUP_SEARCH") ? "groups" :
            "messages";
        const auto records = response.find(arrayName);
        const auto more = response.find("has_more");
        if (records == response.end() || !records->is_array() ||
            records->size() > operation.limit() || more == response.end() ||
            !more->is_boolean()) {
            stopForInvalidResponse();
            return;
        }
        if (name == "HISTORY") {
            std::int64_t watermark = 0;
            std::int64_t next = 0;
            if (!readInteger(response, "through_message_id", watermark) ||
                !readInteger(response, "next_after_message_id", next) ||
                watermark < 0 || next < operation.cursor() || next > watermark ||
                (state.watermark && *state.watermark != watermark)) {
                stopForInvalidResponse();
                return;
            }
            state.watermark = watermark;
        } else if (name == "GROUP_LIST") {
            std::int64_t next = 0;
            if (!validGroupListResult(
                    response, operation.cursor(), operation.limit(), next)) {
                stopForInvalidResponse();
                return;
            }
            for (const json& group : *records)
                cacheGroupName(group["id"], group["name"]);
        } else {
            const bool users = name == "CONTACT_LIST" || name == "USER_SEARCH";
            const char* cursorName = users ? "next_after_user_id" : "next_after_group_id";
            std::int64_t next = 0;
            if (!readInteger(response, cursorName, next) || next < operation.cursor()) {
                stopForInvalidResponse();
                return;
            }
            if ((name == "USER_SEARCH" || name == "GROUP_SEARCH") &&
                response.value("query", std::string()) != state.query) {
                stopForInvalidResponse();
                return;
            }
            if (name == "GROUP_SEARCH") {
                for (const json& group : *records) {
                    std::int64_t id = 0;
                    std::string groupName;
                    if (!readInteger(group, "group_id", id) ||
                        id <= 0 || !readString(group, "name", groupName)) {
                        stopForInvalidResponse(); return;
                    }
                    cacheGroupName(id, groupName);
                }
            }
        }
        state.page = response;
        state.cursor = operation.cursor();
        state.hasPage = true;
        state.scroll = 0;
        if (operation.origin() == ScreenKind::UserSearchInput)
            state.kind = ScreenKind::UserSearchResults;
        else if (operation.origin() == ScreenKind::GroupSearchInput)
            state.kind = ScreenKind::GroupSearchResults;
        return;
    }
    if (name == "CONTACT_ADD") {
        const auto contact = response.find("contact");
        std::string code;
        std::string username;
        std::int64_t id = 0;
        if (!readString(response, "code", code) ||
            (code != "ADDED" && code != "ALREADY_CONTACT") ||
            contact == response.end() || !contact->is_object() ||
            !readString(*contact, "username", username) ||
            !readInteger(*contact, "user_id", id) || id <= 0) {
            stopForInvalidResponse(); return;
        }
        if (state.kind == ScreenKind::UserActions) state.selected["is_contact"] = true;
        else screens.erase(screens.begin() + static_cast<std::ptrdiff_t>(stateIndex));
        setStatus(code == "ADDED" ? "Contact added: " + username
                                   : "Already in contacts: " + username);
        return;
    }
    if (name == "GROUP_CREATE") {
        const auto group = response.find("group");
        std::int64_t id = 0;
        std::string groupName;
        if (group == response.end() || !group->is_object() ||
            !readInteger(*group, "id", id) || id <= 0 ||
            !readString(*group, "name", groupName)) {
            stopForInvalidResponse(); return;
        }
        cacheGroupName(id, groupName);
        screens.erase(screens.begin() + static_cast<std::ptrdiff_t>(stateIndex));
        setStatus("Group created: " + groupName + " (#" + std::to_string(id) + ")");
        return;
    }
    if (name == "GROUP_JOIN") {
        std::int64_t id = 0;
        std::string outcome;
        if (!readInteger(response, "group_id", id) || id <= 0 ||
            !readString(response, "outcome", outcome) ||
            (outcome != "JOINED" && outcome != "ALREADY_MEMBER") ||
            (operation.targetGroupId() && id != *operation.targetGroupId())) {
            stopForInvalidResponse(); return;
        }
        attemptedGroupNames.erase(id);
        if (state.kind == ScreenKind::GroupActions) state.selected["is_member"] = true;
        else screens.erase(screens.begin() + static_cast<std::ptrdiff_t>(stateIndex));
        setStatus(outcome == "JOINED" ? "Group joined: #" + std::to_string(id)
                                       : "Already a member of group #" + std::to_string(id));
        return;
    }
    stopForInvalidResponse();
}

void TerminalUi::applyNameResponse(
    const PendingOperation& operation, const json& response) {
    std::string failure;
    if (responseFailure(response, failure)) {
        if (operation.purpose() == OperationPurpose::NameBootstrap)
            bootstrapActive = false;
        setStatus(failure);
        return;
    }
    const auto groups = response.find("groups");
    std::int64_t next = 0;
    if (!validGroupListResult(
            response, operation.cursor(), operation.limit(), next)) {
        stopForInvalidResponse(); return;
    }
    for (const json& group : *groups) {
        std::int64_t id = 0;
        std::string name;
        if (!readInteger(group, "id", id) || id <= 0 ||
            !readString(group, "name", name)) {
            stopForInvalidResponse(); return;
        }
        cacheGroupName(id, name);
    }
    if (operation.purpose() == OperationPurpose::NameLookup) {
        if (!operation.targetGroupId() || groups->size() > 1 ||
            (!groups->empty() && (*groups)[0]["id"].get<std::int64_t>() !=
                                  *operation.targetGroupId())) {
            setStatus("Group name unavailable");
        }
    } else {
        bootstrapExamined += groups->size();
        bootstrapCursor = next;
        bootstrapActive = response["has_more"].get<bool>() && !groups->empty() &&
            bootstrapExamined < MaxNameEntries && groupNameBytes < MaxNameBytes;
    }
}

void TerminalUi::scheduleNameWork() {
    if (pendingOperation || connection.isStopping()) return;
    const BufferSnapshot snapshot = messageBuffer.snapshot(std::nullopt);
    std::optional<std::int64_t> unknown;
    for (const auto& summary : snapshot.summaries()) {
        if (summary.key().kind() == ConversationKind::Group &&
            groupNames.find(summary.key().groupId()) == groupNames.end() &&
            attemptedGroupNames.find(summary.key().groupId()) == attemptedGroupNames.end() &&
            (!unknown || summary.key().groupId() < *unknown))
            unknown = summary.key().groupId();
    }
    if (unknown) {
        attemptedGroupNames.insert(*unknown);
        const std::int64_t after = *unknown - 1;
        beginOperation(
            PendingOperation(OperationPurpose::NameLookup, "GROUP_LIST",
                             current().kind, after, 1, *unknown),
            {{"type", "GROUP_LIST"}, {"after_group_id", after}, {"limit", 1}});
    } else if (bootstrapActive) {
        beginOperation(
            PendingOperation(OperationPurpose::NameBootstrap, "GROUP_LIST",
                             current().kind, bootstrapCursor, PageLimit),
            {{"type", "GROUP_LIST"}, {"after_group_id", bootstrapCursor},
             {"limit", PageLimit}});
    }
}

bool TerminalUi::groupNamePinned(std::int64_t groupId) const {
    const BufferSnapshot snapshot = messageBuffer.snapshot(std::nullopt);
    for (const auto& summary : snapshot.summaries())
        if (summary.key().kind() == ConversationKind::Group &&
            summary.key().groupId() == groupId) return true;
    return false;
}

void TerminalUi::cacheGroupName(std::int64_t groupId, const std::string& name) {
    if (groupId <= 0 || name.size() > MaxNameBytes) return;
    const auto existing = groupNames.find(groupId);
    if (existing != groupNames.end()) {
        groupNameBytes -= existing->second.first.size();
        existing->second = {name, ++groupNameUse};
        groupNameBytes += name.size();
        dirty = true;
        return;
    }
    while ((groupNames.size() >= MaxNameEntries ||
            name.size() > MaxNameBytes - groupNameBytes) && !groupNames.empty()) {
        auto victim = groupNames.end();
        for (auto candidate = groupNames.begin(); candidate != groupNames.end(); ++candidate) {
            if (!groupNamePinned(candidate->first) &&
                (victim == groupNames.end() ||
                 candidate->second.second < victim->second.second)) victim = candidate;
        }
        if (victim == groupNames.end()) break;
        groupNameBytes -= victim->second.first.size();
        groupNames.erase(victim);
    }
    if (groupNames.size() >= MaxNameEntries ||
        name.size() > MaxNameBytes - groupNameBytes) {
        setStatus("Group name cache capacity reached");
        return;
    }
    groupNames.emplace(groupId, std::make_pair(name, ++groupNameUse));
    groupNameBytes += name.size();
    dirty = true;
}

std::string TerminalUi::groupLabel(std::int64_t groupId) const {
    const auto found = groupNames.find(groupId);
    if (found != groupNames.end()) return found->second.first;
    return "Group #" + std::to_string(groupId) + " (name unavailable)";
}

std::string TerminalUi::conversationLabel(const ConversationKey& key) const {
    if (key.kind() == ConversationKind::Private) return key.username();
    if (key.kind() == ConversationKind::Group) return groupLabel(key.groupId());
    return "Broadcast";
}

std::string TerminalUi::finalSummary() const {
    const BufferSnapshot snapshot = messageBuffer.snapshot(std::nullopt);
    std::size_t unread = 0;
    for (const auto& summary : snapshot.summaries()) unread += summary.unreadCount();
    std::string reason = exitReason.empty() ? connection.stopReason() : exitReason;
    if (reason.empty()) reason = "Client stopped";
    std::string result = "[CLIENT] " + reason + "; " + std::to_string(unread) +
        " session-unread message(s) remain. Acknowledged messages remain in "
        "authorized History; unacknowledged messages can replay after reconnect.";
    if (uncertainAcceptance)
        result += " A submitted message may have been accepted; check History before retrying.";
    return result;
}
