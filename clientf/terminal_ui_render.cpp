#include "terminal_ui.h"

#include <algorithm>
#include <sstream>

namespace {

std::string safe(const std::string& text) {
    return protected_chat::terminal_detail::safeText(text);
}

std::string tailForWidth(const std::string& text, std::size_t width) {
    if (protected_chat::terminal_detail::displayWidth(text) <= width) return text;
    std::size_t offset = 0;
    while (offset < text.size() &&
           protected_chat::terminal_detail::displayWidth(text.substr(offset)) > width) {
        ++offset;
        while (offset < text.size() &&
               (static_cast<unsigned char>(text[offset]) & 0xc0u) == 0x80u)
            ++offset;
    }
    return text.substr(offset);
}

void appendVisible(
    std::vector<std::string>& destination,
    const std::vector<std::string>& source,
    std::size_t height,
    std::size_t scrollFromBottom) {
    if (height == 0) return;
    const std::size_t end = source.size() > scrollFromBottom
        ? source.size() - scrollFromBottom : 0;
    const std::size_t begin = end > height ? end - height : 0;
    destination.insert(destination.end(), source.begin() + begin,
                       source.begin() + end);
}

} // namespace

std::vector<ConversationSummary> TerminalUi::orderedUnread(
    const BufferSnapshot& snapshot) const {
    std::vector<ConversationSummary> result;
    for (const auto& summary : snapshot.summaries())
        if (summary.unreadCount() > 0) result.push_back(summary);
    std::sort(result.begin(), result.end(),
              [](const ConversationSummary& left,
                 const ConversationSummary& right) {
        if (left.latestUnreadSequence() != right.latestUnreadSequence())
            return left.latestUnreadSequence() > right.latestUnreadSequence();
        return left.handle() < right.handle();
    });
    return result;
}

std::string TerminalUi::headerTitle() const {
    switch (current().kind) {
        case ScreenKind::Main: return "Protected Chat";
        case ScreenKind::Messages: return "Messages";
        case ScreenKind::Groups: return "Groups";
        case ScreenKind::Contacts: return "Contacts and Search";
        case ScreenKind::ContactList: return "My Contacts";
        case ScreenKind::GroupList: return "My Groups";
        case ScreenKind::UserSearchInput: return "Search Users";
        case ScreenKind::UserSearchResults: return "User Search";
        case ScreenKind::UserActions: return "User";
        case ScreenKind::GroupSearchInput: return "Search Groups";
        case ScreenKind::GroupSearchResults: return "Group Search";
        case ScreenKind::GroupActions: return "Group";
        case ScreenKind::PrivateTargetInput: return "Private Chat";
        case ScreenKind::ContactAddInput: return "Add Contact";
        case ScreenKind::GroupCreateInput: return "Create Group";
        case ScreenKind::GroupJoinInput: return "Join Group";
        case ScreenKind::History: return "History";
        case ScreenKind::Notifications: return "Notifications";
        case ScreenKind::Conversation:
            if (!current().conversation) return "Conversation";
            if (current().conversation->kind() == ConversationKind::Private)
                return "Chat: " + safe(current().conversation->username());
            if (current().conversation->kind() == ConversationKind::Group)
                return "Group: " + safe(groupLabel(current().conversation->groupId())) +
                    " (#" + std::to_string(current().conversation->groupId()) + ")";
            return "Broadcast — all other registered users";
    }
    return "Protected Chat";
}

std::string TerminalUi::renderInputField(
    const InputField& field, const std::string& label,
    std::size_t width, std::size_t& cursorColumn) const {
    if (width <= label.size() + 2) {
        cursorColumn = width > 0 ? width - 1 : 0;
        return protected_chat::terminal_detail::clipToWidth(label, width);
    }
    const std::size_t available = width - label.size();
    const std::string before = safe(field.text().substr(0, field.cursor()));
    const std::string after = safe(field.text().substr(field.cursor()));
    std::string visibleBefore = before;
    bool clippedLeft = false;
    if (protected_chat::terminal_detail::displayWidth(visibleBefore) >= available) {
        visibleBefore = tailForWidth(before, available > 1 ? available - 1 : 0);
        clippedLeft = true;
    }
    std::string body = (clippedLeft ? "<" : "") + visibleBefore;
    const std::size_t used = protected_chat::terminal_detail::displayWidth(body);
    cursorColumn = label.size() + used;
    if (used < available)
        body += protected_chat::terminal_detail::clipToWidth(after, available - used);
    if (protected_chat::terminal_detail::displayWidth(before + after) > available &&
        protected_chat::terminal_detail::displayWidth(body) >= available) {
        body = protected_chat::terminal_detail::clipToWidth(
            body, available > 0 ? available - 1 : 0) + ">";
    }
    return label + body;
}

std::vector<std::string> TerminalUi::renderConversation(
    std::size_t width, std::size_t height,
    const BufferSnapshot& snapshot) {
    std::vector<std::string> wrapped;
    std::vector<std::pair<std::int64_t, std::size_t>> anchors;
    if (snapshot.messages().empty()) {
        wrapped.push_back("Session messages; earlier accepted messages are in History");
        anchors.emplace_back(0, 0);
    }
    for (const auto& message : snapshot.messages()) {
        std::string prefix = message->outgoing() ? "You > " : safe(message->sender()) + " > ";
        const std::string line = prefix + safe(message->content());
        const auto rows = protected_chat::terminal_detail::wrapToWidth(line, width);
        std::size_t sourceByteOffset = 0;
        for (const auto& row : rows) {
            wrapped.push_back(row);
            anchors.emplace_back(message->messageId(), sourceByteOffset);
            sourceByteOffset += row.size();
        }
    }
    auto& state = conversationState(*current().conversation);
    std::size_t start = wrapped.size() > height ? wrapped.size() - height : 0;
    if (!state.followLatest) {
        if (state.anchorMessageId) {
            std::size_t match = 0;
            bool found = false;
            for (std::size_t index = 0; index < anchors.size(); ++index) {
                if (anchors[index].first == *state.anchorMessageId &&
                    anchors[index].second <= state.anchorSourceByteOffset) {
                    match = index;
                    found = true;
                }
            }
            if (found) start = match;
        } else {
            const std::size_t end = state.scroll >= wrapped.size()
                ? 0 : wrapped.size() - state.scroll;
            start = end > height ? end - height : 0;
        }
        if (state.anchorMoveRows < 0) {
            const std::size_t amount = static_cast<std::size_t>(-state.anchorMoveRows);
            start = start > amount ? start - amount : 0;
        } else if (state.anchorMoveRows > 0) {
            start = std::min(
                start + static_cast<std::size_t>(state.anchorMoveRows),
                wrapped.size() > height ? wrapped.size() - height : 0);
        }
        state.anchorMoveRows = 0;
        if (start + height >= wrapped.size()) {
            state.followLatest = true;
            state.scroll = 0;
            state.anchorMessageId.reset();
        } else if (!anchors.empty()) {
            state.anchorMessageId = anchors[start].first;
            state.anchorSourceByteOffset = anchors[start].second;
        }
    }
    std::vector<std::string> result;
    for (std::size_t index = start;
         index < wrapped.size() && result.size() < height; ++index)
        result.push_back(wrapped[index]);
    bool hasUnread = false;
    for (const auto& summary : snapshot.summaries()) {
        if (summary.key() == *current().conversation && summary.unreadCount() > 0)
            hasUnread = true;
    }
    if (!state.followLatest && hasUnread) {
        const std::string notice = "New unread messages — Ctrl+End";
        if (result.size() < height) result.push_back(notice);
        else if (!result.empty()) result.back() = notice;
    }
    return result;
}

std::vector<std::string> TerminalUi::renderNotifications(
    std::size_t width, std::size_t height,
    const BufferSnapshot& snapshot) const {
    std::vector<std::string> all;
    for (const auto& summary : orderedUnread(snapshot)) {
        all.push_back(std::to_string(summary.handle()) + ". " +
            safe(conversationLabel(summary.key())) + " (" +
            std::to_string(summary.unreadCount()) + ")");
    }
    if (all.empty()) all.push_back("No unread session conversations");
    for (auto& line : all)
        line = protected_chat::terminal_detail::clipToWidth(line, width);
    std::vector<std::string> result;
    appendVisible(result, all, height, current().scroll);
    return result;
}

std::vector<std::string> TerminalUi::renderResultPage(
    std::size_t width, std::size_t height) const {
    std::vector<std::string> all;
    const ScreenState& state = current();
    const nlohmann::json* records = nullptr;
    if (!state.hasPage) {
        all.push_back("Waiting for server response...");
    } else if (state.kind == ScreenKind::ContactList) {
        records = &state.page["contacts"];
        for (std::size_t index = 0; index < records->size(); ++index)
            all.push_back(std::to_string(index + 1) + ". " +
                safe((*records)[index]["username"]) + " (User ID: " +
                std::to_string((*records)[index]["user_id"].get<std::int64_t>()) + ")");
    } else if (state.kind == ScreenKind::GroupList) {
        records = &state.page["groups"];
        for (std::size_t index = 0; index < records->size(); ++index)
            all.push_back(std::to_string(index + 1) + ". " +
                safe((*records)[index]["name"]) + " (#" +
                std::to_string((*records)[index]["id"].get<std::int64_t>()) + ")");
    } else if (state.kind == ScreenKind::UserSearchResults) {
        records = &state.page["users"];
        for (std::size_t index = 0; index < records->size(); ++index)
            all.push_back(std::to_string(index + 1) + ". " +
                safe((*records)[index]["username"]) + " (User ID: " +
                std::to_string((*records)[index]["user_id"].get<std::int64_t>()) +
                ((*records)[index]["is_contact"].get<bool>() ? ", contact)" : ", not a contact)"));
    } else if (state.kind == ScreenKind::GroupSearchResults) {
        records = &state.page["groups"];
        for (std::size_t index = 0; index < records->size(); ++index)
            all.push_back(std::to_string(index + 1) + ". " +
                safe((*records)[index]["name"]) + " (#" +
                std::to_string((*records)[index]["group_id"].get<std::int64_t>()) +
                ((*records)[index]["is_member"].get<bool>() ? ", member)" : ", not a member)"));
    } else if (state.kind == ScreenKind::History) {
        records = &state.page["messages"];
        for (const auto& message : *records) {
            std::string line = "[HISTORY #" +
                std::to_string(message["message_id"].get<std::int64_t>()) + " " +
                safe(message["kind"]) + "] " + safe(message["sender"]);
            if (message["kind"] == "GROUP")
                line += " in group " + std::to_string(message["group_id"].get<std::int64_t>());
            line += " at " + std::to_string(message["created_at"].get<std::int64_t>()) +
                ": " + safe(message["content"]);
            const auto wrapped = protected_chat::terminal_detail::wrapToWidth(line, width);
            all.insert(all.end(), wrapped.begin(), wrapped.end());
        }
    }
    if (records != nullptr && records->empty()) all.push_back("No results");
    if (state.hasPage && state.page.value("has_more", false)) {
        const std::size_t next = state.kind == ScreenKind::History ? 1 : records->size() + 1;
        all.push_back(std::to_string(next) + ". Next Page");
    } else if (state.hasPage) {
        all.push_back(state.kind == ScreenKind::History ? "End of history" : "End of results");
    }
    all.push_back("0. Back");
    for (auto& line : all)
        line = protected_chat::terminal_detail::clipToWidth(line, width);
    std::vector<std::string> result;
    appendVisible(result, all, height, state.scroll);
    return result;
}

ScreenFrame TerminalUi::renderCurrentScreen(
    const TerminalSize& size, const BufferSnapshot& snapshot) {
    const std::size_t rows = size.rows();
    const std::size_t columns = size.columns();
    std::vector<std::string> frameRows;
    if (terminalTooSmall(size)) {
        std::size_t unread = 0;
        for (const auto& summary : snapshot.summaries()) unread += summary.unreadCount();
        frameRows = {"Terminal too small — need 40x12",
                     "Unread: " + std::to_string(unread),
                     "Ctrl+Q / Ctrl+C / Ctrl+D Exit"};
        return ScreenFrame(std::move(frameRows), 0, 0, false);
    }
    const bool wide = columns >= 100 && rows >= 20;
    const std::size_t sidebarWidth = wide ? 28 : 0;
    const std::size_t contentWidth = columns - (wide ? sidebarWidth + 1 : 0) - 1;
    const std::size_t topRows = wide ? 1 : 3;
    const std::size_t bodyHeight = rows > topRows + 4 ? rows - topRows - 4 : 1;
    frameRows.push_back("Protected Chat | " + headerTitle() + " | Connected");
    const auto unread = orderedUnread(snapshot);
    if (!wide) {
        std::size_t total = 0;
        for (const auto& summary : unread) total += summary.unreadCount();
        frameRows.push_back("Notifications: " + std::to_string(total));
        std::string labels;
        for (std::size_t index = 0; index < std::min<std::size_t>(2, unread.size()); ++index) {
            if (!labels.empty()) labels += " | ";
            labels += safe(conversationLabel(unread[index].key())) + " (" +
                std::to_string(unread[index].unreadCount()) + ")";
        }
        if (unread.size() > 2) labels += " | +" + std::to_string(unread.size() - 2) + " more";
        frameRows.push_back(labels);
    }
    std::vector<std::string> body;
    const ScreenKind kind = current().kind;
    if (kind == ScreenKind::Conversation)
        body = renderConversation(contentWidth, bodyHeight, snapshot);
    else if (kind == ScreenKind::Notifications)
        body = renderNotifications(contentWidth, bodyHeight, snapshot);
    else if (kind == ScreenKind::ContactList || kind == ScreenKind::GroupList ||
             kind == ScreenKind::UserSearchResults ||
             kind == ScreenKind::GroupSearchResults || kind == ScreenKind::History)
        body = renderResultPage(contentWidth, bodyHeight);
    else {
        if (kind == ScreenKind::Main)
            body = {"1. Messages", "2. Groups", "3. Contacts and Search",
                    "4. History", "5. Notifications", "6. Exit"};
        else if (kind == ScreenKind::Messages)
            body = {"1. Private Chat from Contacts", "2. Private Chat by Username",
                    "3. Group Chat from My Groups", "4. Broadcast", "0. Back"};
        else if (kind == ScreenKind::Groups)
            body = {"1. My Groups", "2. Create Group", "3. Search and Join Groups",
                    "4. Join Group by ID", "0. Back"};
        else if (kind == ScreenKind::Contacts)
            body = {"1. My Contacts", "2. Add Contact by Username",
                    "3. Search Users", "0. Back"};
        else if (kind == ScreenKind::UserActions) {
            body = {safe(current().selected.value("username", std::string())) +
                    " (User ID: " + std::to_string(current().selected.value("user_id", 0)) + ")",
                    "1. Open Private Chat"};
            if (!current().selected.value("is_contact", false)) body.push_back("2. Add to Contacts");
            body.push_back("0. Back");
        } else if (kind == ScreenKind::GroupActions) {
            body = {safe(current().selected.value("name", std::string())) + " (#" +
                    std::to_string(current().selected.value("group_id", 0)) + ")",
                    current().selected.value("is_member", false)
                        ? "1. Open Group Chat" : "1. Join Group", "0. Back"};
        } else {
            body.push_back(kind == ScreenKind::PrivateTargetInput ? "Recipient username" :
                kind == ScreenKind::ContactAddInput ? "Contact username" :
                kind == ScreenKind::GroupCreateInput ? "Group name" :
                kind == ScreenKind::GroupJoinInput ? "Positive Group ID" :
                kind == ScreenKind::UserSearchInput ? "User search query" :
                "Group search query");
            body.push_back("Enter Submit · Esc Back");
        }
    }
    body.resize(std::min(body.size(), bodyHeight));
    while (body.size() < bodyHeight) body.emplace_back();
    if (wide) {
        for (std::size_t row = 0; row < bodyHeight; ++row) {
            std::string left = protected_chat::terminal_detail::clipToWidth(body[row], contentWidth);
            const std::size_t leftWidth = protected_chat::terminal_detail::displayWidth(left);
            if (leftWidth < contentWidth) left.append(contentWidth - leftWidth, ' ');
            std::string side;
            if (row == 0) side = "Notifications — Ctrl+N";
            else if (row - 1 < unread.size())
                side = safe(conversationLabel(unread[row - 1].key())) + " (" +
                    std::to_string(unread[row - 1].unreadCount()) + ")";
            else if (row > 1 && row - 1 == std::min(unread.size(), bodyHeight - 1) &&
                     unread.size() > bodyHeight - 1)
                side = "+" + std::to_string(unread.size() - (bodyHeight - 1)) + " more — Ctrl+N";
            frameRows.push_back(left + "|" +
                protected_chat::terminal_detail::clipToWidth(side, sidebarWidth));
        }
    } else frameRows.insert(frameRows.end(), body.begin(), body.end());
    frameRows.push_back(std::string(std::min(columns - 1, std::size_t(80)), '-'));
    std::string latestDiagnostic;
    {
        std::lock_guard<std::mutex> lock(responseMutex);
        latestDiagnostic = diagnostic;
    }
    frameRows.push_back("Status: " + safe(statusMessage.empty() ? latestDiagnostic : statusMessage));
    std::size_t cursorColumn = 0;
    const InputField* field = activeInput();
    const std::string label = kind == ScreenKind::Conversation ? "Message: " : "Select/Input: ";
    frameRows.push_back(field ? renderInputField(*field, label, columns - 1, cursorColumn) : label);
    frameRows.push_back(kind == ScreenKind::Conversation
        ? "Enter Send · Esc Back · Ctrl+N Notifications · Ctrl+Q Exit"
        : "Enter Select · Esc Back · Ctrl+N Notifications · Ctrl+Q Exit");
    return ScreenFrame(std::move(frameRows), rows - 2, cursorColumn, true);
}
