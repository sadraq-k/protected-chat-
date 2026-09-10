#ifndef PROTECTED_CHAT_TERMINAL_UI_H
#define PROTECTED_CHAT_TERMINAL_UI_H

#include "client_connection.h"
#include "console_input.h"
#include "message_buffer.h"
#include "terminal_screen.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class ConsoleOutput {
public:
    ConsoleOutput() = default;
    ConsoleOutput(const ConsoleOutput&) = delete;
    ConsoleOutput& operator=(const ConsoleOutput&) = delete;
    ConsoleOutput(ConsoleOutput&&) = delete;
    ConsoleOutput& operator=(ConsoleOutput&&) = delete;
    void line(const std::string& text);
    void error(const std::string& text);
    void prompt(const std::string& text);
    void clearPrompt();
private:
    std::mutex mutex;
    std::string activePrompt;
};

class InputField {
public:
    static constexpr std::size_t MaxBytes = 65'536;
    bool insert(const std::string& text);
    bool eraseBefore();
    bool eraseAt();
    bool moveLeft();
    bool moveRight();
    void home() noexcept;
    void end() noexcept;
    void clear();
    bool replace(const std::string& text);
    const std::string& text() const noexcept;
    std::size_t cursor() const noexcept;
    std::uint64_t revision() const noexcept;
private:
    std::string bytes;
    std::size_t byteCursor = 0;
    std::uint64_t editRevision = 0;
};

enum class ScreenKind {
    Main, Messages, Groups, Contacts, ContactList, GroupList,
    UserSearchInput, UserSearchResults, UserActions,
    GroupSearchInput, GroupSearchResults, GroupActions,
    PrivateTargetInput, ContactAddInput, GroupCreateInput, GroupJoinInput,
    History, Notifications, Conversation
};

enum class OperationPurpose { Foreground, NameBootstrap, NameLookup };

class ScreenState {
public:
    explicit ScreenState(ScreenKind kind = ScreenKind::Main);
    ScreenKind kind;
    InputField input;
    std::optional<ConversationKey> conversation;
    nlohmann::json page;
    nlohmann::json selected;
    std::string query;
    std::int64_t cursor = 0;
    std::optional<std::int64_t> watermark;
    std::size_t scroll = 0;
    bool hasPage = false;
};

class PendingOperation {
public:
    PendingOperation(
        OperationPurpose purpose,
        std::string operation,
        ScreenKind origin,
        std::int64_t cursor,
        std::size_t limit,
        std::optional<std::int64_t> targetGroupId = std::nullopt);
    OperationPurpose purpose() const noexcept;
    const std::string& operation() const noexcept;
    ScreenKind origin() const noexcept;
    std::int64_t cursor() const noexcept;
    std::size_t limit() const noexcept;
    const std::optional<std::int64_t>& targetGroupId() const noexcept;
private:
    OperationPurpose operationPurpose;
    std::string operationName;
    ScreenKind originScreen;
    std::int64_t requestCursor;
    std::size_t requestLimit;
    std::optional<std::int64_t> lookupGroupId;
};

class TerminalUi {
public:
    TerminalUi(ClientConnection& connection, ConsoleInput& input,
               ConsoleOutput& output);
    ~TerminalUi() = default;
    TerminalUi(const TerminalUi&) = delete;
    TerminalUi& operator=(const TerminalUi&) = delete;
    TerminalUi(TerminalUi&&) = delete;
    TerminalUi& operator=(TerminalUi&&) = delete;

    void setAuthenticatedUsername(const std::string& username);
    void handleFrame(ClientFrameKind kind, const nlohmann::json& frame);
    void handleDiagnostic(const std::string& message, bool error);
    void run(TerminalScreen& terminal);
    std::string finalSummary() const;

private:
    class ConversationUiState {
    public:
        InputField draft;
        std::string lastFailed;
        std::size_t scroll = 0;
        std::optional<std::int64_t> anchorMessageId;
        std::size_t anchorSourceByteOffset = 0;
        int anchorMoveRows = 0;
        bool followLatest = true;
    };

    static constexpr std::size_t PageLimit = 50;
    static constexpr std::size_t MaxScreenDepth = 8;
    static constexpr std::size_t MaxDiagnosticBytes = 1024;
    static constexpr std::size_t MaxNameEntries = 2048;
    static constexpr std::size_t MaxNameBytes = 4 * 1024 * 1024;

    ScreenState& current();
    const ScreenState& current() const;
    bool push(ScreenState state);
    void back();
    void openNotifications();
    void openConversation(const ConversationKey& key);
    ConversationUiState& conversationState(const ConversationKey& key);

    void processEvent(const ConsoleEvent& event);
    void processEnter();
    void processEscape();
    void processText(const std::string& text, bool paste);
    void processEditingKey(ConsoleKey key);
    bool submitMenu(std::size_t choice);
    bool submitForm();
    bool submitConversation();
    bool submitListSelection(std::size_t choice);
    bool beginOperation(
        PendingOperation operation,
        const nlohmann::json& request);
    void consumeResponse();
    void applyForegroundResponse(
        const PendingOperation& operation,
        const nlohmann::json& response);
    void applyNameResponse(
        const PendingOperation& operation,
        const nlohmann::json& response);
    void clearOperation();
    void stopForInvalidResponse();
    bool responseFailure(const nlohmann::json& response, std::string& detail);
    void scheduleNameWork();
    void cacheGroupName(std::int64_t groupId, const std::string& name);
    std::string groupLabel(std::int64_t groupId) const;
    bool groupNamePinned(std::int64_t groupId) const;

    ScreenFrame renderCurrentScreen(
        const TerminalSize& size, const BufferSnapshot& snapshot);
    std::vector<std::string> renderConversation(
        std::size_t width, std::size_t height,
        const BufferSnapshot& snapshot);
    std::vector<std::string> renderResultPage(
        std::size_t width, std::size_t height) const;
    std::vector<std::string> renderNotifications(
        std::size_t width, std::size_t height,
        const BufferSnapshot& snapshot) const;
    std::string renderInputField(
        const InputField& field, const std::string& label,
        std::size_t width, std::size_t& cursorColumn) const;
    std::vector<ConversationSummary> orderedUnread(
        const BufferSnapshot& snapshot) const;
    std::string conversationLabel(const ConversationKey& key) const;
    std::string headerTitle() const;
    InputField* activeInput();
    const InputField* activeInput() const;
    bool activeInputIsMultiline() const;
    bool terminalTooSmall(const TerminalSize& size) const noexcept;
    void setStatus(std::string message);
    void requestExit(std::string reason);

    ClientConnection& connection;
    ConsoleInput& input;
    ConsoleOutput& output;
    MessageBuffer messageBuffer;
    mutable std::mutex responseMutex;
    std::string expectedOperation;
    std::optional<nlohmann::json> pendingResponse;
    std::optional<PendingOperation> pendingOperation;
    std::string diagnostic;
    std::size_t diagnosticCount = 0;
    bool diagnosticIsError = false;
    std::vector<ScreenState> screens;
    std::map<ConversationKey, ConversationUiState> conversationStates;
    std::map<std::int64_t, std::pair<std::string, std::uint64_t>> groupNames;
    std::set<std::int64_t> attemptedGroupNames;
    std::size_t groupNameBytes = 0;
    std::uint64_t groupNameUse = 0;
    bool bootstrapActive = true;
    std::int64_t bootstrapCursor = 0;
    std::size_t bootstrapExamined = 0;
    std::string authenticatedUsername;
    std::string statusMessage;
    std::string exitReason;
    std::optional<ConversationKey> submittedKey;
    std::string submittedText;
    std::uint64_t submittedRevision = 0;
    bool uncertainAcceptance = false;
    bool exitRequested = false;
    bool dirty = true;
    bool lastScreenTooSmall = false;
    bool terminalSizeWarningActive = false;
};

#endif
