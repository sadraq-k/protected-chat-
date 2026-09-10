#ifndef PROTECTED_CHAT_TERMINAL_UI_H
#define PROTECTED_CHAT_TERMINAL_UI_H

#include "client_connection.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

class ConsoleInput;

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
    void event(const std::string& text);

private:
    std::mutex mutex;
    std::string activePrompt;
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

    void handleFrame(ClientFrameKind kind, const nlohmann::json& frame);
    void run();

private:
    bool readLine(const std::string& prompt, std::string& line);
    bool readChoice(
        const std::string& menu,
        std::size_t maximum,
        bool allowBack,
        std::size_t& choice);
    bool requestFits(const nlohmann::json& request);
    bool performRequest(
        const std::string& operation,
        const nlohmann::json& request,
        nlohmann::json& response);
    bool waitForResponse(nlohmann::json& response);
    void clearOutstandingResponse();
    void stopForInvalidResponse();
    bool showFailure(const nlohmann::json& response);

    void mainMenu();
    void messagesMenu();
    void groupsMenu();
    void contactsMenu();
    void contactList();
    void groupList();
    void userSearch();
    void groupSearch();
    void history();
    void addContactPrompt();
    void createGroupPrompt();
    void joinGroupPrompt();
    void userActions(nlohmann::json& user);
    void groupActions(nlohmann::json& group);
    void composer(
        const std::string& kind,
        const std::string& targetName,
        const std::optional<std::int64_t>& groupId);

    ClientConnection& connection;
    ConsoleInput& input;
    ConsoleOutput& output;
    std::mutex responseMutex;
    std::string expectedOperation;
    std::optional<nlohmann::json> pendingResponse;
    bool exitRequested;
};

#endif
