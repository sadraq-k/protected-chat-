#include "clientf/client_connection.h"
#include "clientf/console_input.h"
#include "clientf/terminal_ui.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

using json = nlohmann::json;

#ifndef PROTECTED_CHAT_CLIENT_HOST
#define PROTECTED_CHAT_CLIENT_HOST "194.9.56.182"
#endif

#ifndef PROTECTED_CHAT_CLIENT_PORT
#define PROTECTED_CHAT_CLIENT_PORT "1403"
#endif

namespace {

bool readConsoleLine(
    ConsoleInput& input,
    ConsoleOutput& output,
    ClientConnection& connection,
    const std::string& prompt,
    std::string& line) {
    output.prompt(prompt);
    ConsoleReadResult result = input.readLine(line);
    output.clearPrompt();
    while (result == ConsoleReadResult::WorkAvailable) {
        if (!connection.flushAutomaticRequests()) {
            return false;
        }
        output.prompt(prompt);
        result = input.readLine(line);
        output.clearPrompt();
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

bool sendAuthenticationRequest(
    ClientConnection& connection,
    ConsoleOutput& output,
    const json& request) {
    const ClientSendResult result = connection.sendJson(request);
    if (result == ClientSendResult::Written) {
        output.line("[SEND] Authentication request frame written");
        return true;
    }
    if (result == ClientSendResult::TooLarge) {
        output.error(
            "[ERROR] Authentication request exceeds the server's "
            "65,536-byte request limit");
        return false;
    }
    output.error("[ERROR] Authentication request write failed");
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
        output.line("[AUTH] Registration requested");
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
        output.line("[AUTH] Login requested");
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

void runClient(const std::string& host, const std::string& port) {
    ConsoleInput input;
    ConsoleOutput output;
    ClientConnection connection;
    TerminalUi ui(connection, input, output);

    connection.setAutomaticRequestNotifier([&input] { input.notifyWork(); });

    output.line("[CLIENT] Connecting to " + host + ":" + port);
    try {
        connection.connect(host, port);
    } catch (const boost::system::system_error& error) {
        output.error("[ERROR] Connection failed: " + error.code().message());
        return;
    }
    output.line("[CLIENT] Connected to server");

    try {
        connection.startReceiver(
            [&ui](ClientFrameKind kind, const json& message) {
                ui.handleFrame(kind, message);
            },
            [&output](const std::string& message, bool error) {
                output.event(
                    std::string(error ? "[ERROR] " : "[CLIENT] ") + message);
            },
            [&input] { input.interrupt(); });
    } catch (const std::system_error& error) {
        output.error(
            "[ERROR] Could not start receiver: " +
            std::string(error.what()));
        finishConnection(connection, "Receiver thread startup failed");
        return;
    }

    try {
        json authenticationRequest;
        if (!buildAuthenticationRequest(
                input, output, connection, authenticationRequest)) {
            finishConnection(connection, "Authentication input ended");
            return;
        }
        if (!sendAuthenticationRequest(
                connection, output, authenticationRequest)) {
            finishConnection(
                connection, "Authentication request was not written");
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
            finishConnection(
                connection, "Initial pending synchronization failed");
            return;
        }

        ui.run();
        if (connection.isAuthenticated() && !connection.isStopping()) {
            connection.sendJson({{"type", "EXIT"}});
        }
        finishConnection(connection, "Client stopped");
        output.line("[CLIENT] Connection closed");
    } catch (...) {
        finishConnection(connection, "Client exception");
        throw;
    }
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
