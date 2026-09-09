#include "durable_routing.h"

#include "client_session.h"
#include "database.h"
#include "session_registry.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>

using json = nlohmann::json;

namespace {

constexpr std::size_t DefaultPageLimit = 50;
constexpr std::size_t MaximumPageLimit = 50;
constexpr std::size_t MaximumResponseBytes = 1'048'576;

bool containsOnlyFields(
    const json& request,
    std::initializer_list<const char*> allowedFields) {
    for (const auto& field : request.items()) {
        bool allowed = false;
        for (const char* allowedField : allowedFields) {
            if (field.key() == allowedField) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool readNonEmptyString(
    const json& request,
    const char* field,
    std::string& value) {
    const auto found = request.find(field);
    if (found == request.end() || !found->is_string()) {
        return false;
    }
    value = found->get<std::string>();
    return !value.empty();
}

bool readSignedInteger(
    const json& request,
    const char* field,
    std::int64_t& value) {
    const auto found = request.find(field);
    if (found == request.end()) {
        return false;
    }
    if (found->is_number_unsigned()) {
        const std::uint64_t unsignedValue = found->get<std::uint64_t>();
        if (unsignedValue >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        value = static_cast<std::int64_t>(unsignedValue);
        return true;
    }
    if (!found->is_number_integer()) {
        return false;
    }
    value = found->get<std::int64_t>();
    return true;
}

DurableRequestResult writeResponse(
    const std::shared_ptr<ClientSession>& session,
    const json& response) {
    return session->sendJson(response) == SendResult::Written
        ? DurableRequestResult::Handled
        : DurableRequestResult::Stop;
}

DurableRequestResult writeFailure(
    const std::shared_ptr<ClientSession>& session,
    const std::string& operation,
    const std::string& code,
    const std::string& message,
    const std::optional<std::int64_t>& messageId = std::nullopt) {
    json response = {
        {"status", "FAIL"},
        {"operation", operation},
        {"code", code},
        {"message", message}
    };
    if (messageId) {
        response["message_id"] = *messageId;
    }
    return writeResponse(session, response);
}

std::string kindText(DurableMessageKind kind) {
    switch (kind) {
        case DurableMessageKind::Private:
            return "PRIVATE";
        case DurableMessageKind::Group:
            return "GROUP";
        case DurableMessageKind::Broadcast:
            return "BROADCAST";
    }
    throw std::invalid_argument("Invalid durable message kind");
}

json deliveryEnvelope(const DeliveryMessage& delivery) {
    const StoredMessage& stored = delivery.message();
    json envelope = {
        {"type", "MESSAGE"},
        {"message_id", stored.id()},
        {"kind", kindText(stored.kind())},
        {"sender_id", stored.senderId()},
        {"sender", delivery.senderUsername()},
        {"content", stored.content()},
        {"created_at", stored.createdAt()}
    };
    if (stored.kind() == DurableMessageKind::Group) {
        if (!stored.groupId()) {
            throw std::runtime_error("Stored Group message lacks a group ID");
        }
        envelope["group_id"] = *stored.groupId();
    }
    return envelope;
}

void deliverCommittedMessage(
    const AcceptedMessage& accepted,
    SessionRegistry& registry) {
    json envelope;
    try {
        envelope = deliveryEnvelope(accepted.message());
        if (envelope.dump().size() > MaximumResponseBytes) {
            std::cerr << "[DELIVERY] Committed message is not representable"
                      << std::endl;
            return;
        }
    } catch (const std::exception&) {
        std::cerr << "[DELIVERY] Committed message is not representable"
                  << std::endl;
        return;
    }

    for (const RecipientRoute& route : accepted.recipients()) {
        const std::shared_ptr<ClientSession> target =
            registry.findReady(route.username());
        if (!target) {
            continue;
        }
        if (target->sendJson(envelope) == SendResult::Written) {
            continue;
        }
        std::cerr << "[DELIVERY] Ready-recipient write failed" << std::endl;
        target->requestStop();
        registry.removeIfSame(route.username(), target);
    }
}

DurableRequestResult acceptanceFailure(
    const std::shared_ptr<ClientSession>& session,
    const std::string& operation,
    MessageAcceptanceStatus status) {
    switch (status) {
        case MessageAcceptanceStatus::SenderNotFound:
            return writeFailure(
                session, operation, "SENDER_NOT_FOUND", "Sender not found");
        case MessageAcceptanceStatus::RecipientNotFound:
            return writeFailure(
                session,
                operation,
                "RECIPIENT_NOT_FOUND",
                "Recipient not found");
        case MessageAcceptanceStatus::SelfMessageNotAllowed:
            return writeFailure(
                session,
                operation,
                "SELF_MESSAGE_NOT_ALLOWED",
                "Self-messaging is not allowed");
        case MessageAcceptanceStatus::GroupNotFound:
            return writeFailure(
                session, operation, "GROUP_NOT_FOUND", "Group not found");
        case MessageAcceptanceStatus::SenderNotMember:
            return writeFailure(
                session,
                operation,
                "SENDER_NOT_MEMBER",
                "Sender is not a group member");
        case MessageAcceptanceStatus::Accepted:
            break;
    }
    return writeFailure(
        session, operation, "DATABASE_ERROR", "Database operation failed");
}

DurableRequestResult finishAcceptance(
    const std::shared_ptr<ClientSession>& session,
    SessionRegistry& registry,
    const std::string& operation,
    MessageAcceptanceResult result) {
    if (result.status() != MessageAcceptanceStatus::Accepted) {
        return acceptanceFailure(session, operation, result.status());
    }
    if (!result.acceptedMessage()) {
        return writeFailure(
            session, operation, "DATABASE_ERROR", "Database operation failed");
    }

    const AcceptedMessage& accepted = *result.acceptedMessage();
    const StoredMessage& stored = accepted.message().message();
    const json response = {
        {"status", "SUCCESS"},
        {"operation", operation},
        {"code", "ACCEPTED"},
        {"message", "Message accepted"},
        {"message_id", stored.id()},
        {"kind", kindText(stored.kind())},
        {"recipient_count", accepted.recipients().size()}
    };
    const bool senderWritten =
        session->sendJson(response) == SendResult::Written;
    deliverCommittedMessage(accepted, registry);
    return senderWritten
        ? DurableRequestResult::Handled
        : DurableRequestResult::Stop;
}

DurableRequestResult handlePrivate(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database,
    SessionRegistry& registry) {
    std::string receiver;
    std::string content;
    if (!containsOnlyFields(request, {"type", "receiver", "content"}) ||
        !readNonEmptyString(request, "receiver", receiver) ||
        !readNonEmptyString(request, "content", content)) {
        return writeFailure(
            session, "PRIVATE", "INVALID_REQUEST", "Invalid request");
    }
    try {
        return finishAcceptance(
            session,
            registry,
            "PRIVATE",
            database.acceptPrivateMessage(
                session->authenticatedUsername(), receiver, content));
    } catch (const std::invalid_argument&) {
        return writeFailure(
            session, "PRIVATE", "INVALID_REQUEST", "Invalid request");
    } catch (const std::runtime_error&) {
        std::cerr << "[DATABASE] Private acceptance failed" << std::endl;
        return writeFailure(
            session,
            "PRIVATE",
            "DATABASE_ERROR",
            "Database operation failed");
    }
}

DurableRequestResult handleGroup(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database,
    SessionRegistry& registry) {
    std::int64_t groupId = 0;
    std::string content;
    if (!containsOnlyFields(request, {"type", "group_id", "content"}) ||
        !readSignedInteger(request, "group_id", groupId) || groupId <= 0 ||
        !readNonEmptyString(request, "content", content)) {
        return writeFailure(
            session, "GROUP", "INVALID_REQUEST", "Invalid request");
    }
    try {
        return finishAcceptance(
            session,
            registry,
            "GROUP",
            database.acceptGroupMessage(
                session->authenticatedUsername(), groupId, content));
    } catch (const std::invalid_argument&) {
        return writeFailure(
            session, "GROUP", "INVALID_REQUEST", "Invalid request");
    } catch (const std::runtime_error&) {
        std::cerr << "[DATABASE] Group acceptance failed" << std::endl;
        return writeFailure(
            session, "GROUP", "DATABASE_ERROR", "Database operation failed");
    }
}

DurableRequestResult handleBroadcast(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database,
    SessionRegistry& registry) {
    std::string content;
    if (!containsOnlyFields(request, {"type", "content"}) ||
        !readNonEmptyString(request, "content", content)) {
        return writeFailure(
            session, "BROADCAST", "INVALID_REQUEST", "Invalid request");
    }
    try {
        return finishAcceptance(
            session,
            registry,
            "BROADCAST",
            database.acceptBroadcastMessage(
                session->authenticatedUsername(), content));
    } catch (const std::invalid_argument&) {
        return writeFailure(
            session, "BROADCAST", "INVALID_REQUEST", "Invalid request");
    } catch (const std::runtime_error&) {
        std::cerr << "[DATABASE] Broadcast acceptance failed" << std::endl;
        return writeFailure(
            session,
            "BROADCAST",
            "DATABASE_ERROR",
            "Database operation failed");
    }
}

DurableRequestResult handleAcknowledgement(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database) {
    std::int64_t messageId = 0;
    if (!containsOnlyFields(request, {"type", "message_id"}) ||
        !readSignedInteger(request, "message_id", messageId) || messageId <= 0) {
        return writeFailure(
            session,
            "DELIVERY_ACK",
            "INVALID_REQUEST",
            "Invalid request");
    }
    try {
        const AcknowledgementResult result = database.acknowledgeDelivery(
            session->authenticatedUsername(), messageId);
        if (result == AcknowledgementResult::NotFoundOrNotRecipient) {
            return writeFailure(
                session,
                "DELIVERY_ACK",
                "NOT_FOUND_OR_NOT_RECIPIENT",
                "Message is not available for acknowledgement",
                messageId);
        }
        return writeResponse(session, {
            {"status", "SUCCESS"},
            {"operation", "DELIVERY_ACK"},
            {"code", result == AcknowledgementResult::Acknowledged
                ? "ACKNOWLEDGED" : "ALREADY_ACKNOWLEDGED"},
            {"message", result == AcknowledgementResult::Acknowledged
                ? "Delivery acknowledged" : "Delivery already acknowledged"},
            {"message_id", messageId}
        });
    } catch (const std::invalid_argument&) {
        return writeFailure(
            session,
            "DELIVERY_ACK",
            "INVALID_REQUEST",
            "Invalid request");
    } catch (const std::runtime_error&) {
        std::cerr << "[DATABASE] Delivery acknowledgement failed" << std::endl;
        return writeFailure(
            session,
            "DELIVERY_ACK",
            "DATABASE_ERROR",
            "Database operation failed",
            messageId);
    }
}

bool readPageRequest(
    const json& request,
    std::int64_t& afterMessageId,
    std::optional<std::int64_t>& throughMessageId,
    std::size_t& limit) {
    if (!containsOnlyFields(
            request,
            {"type", "after_message_id", "through_message_id", "limit"})) {
        return false;
    }
    afterMessageId = 0;
    const auto after = request.find("after_message_id");
    if (after != request.end() &&
        (!readSignedInteger(request, "after_message_id", afterMessageId) ||
         afterMessageId < 0)) {
        return false;
    }
    const auto through = request.find("through_message_id");
    if (through != request.end()) {
        std::int64_t value = 0;
        if (!readSignedInteger(request, "through_message_id", value) ||
            value < 0 || afterMessageId > value) {
            return false;
        }
        throughMessageId = value;
    } else if (afterMessageId != 0) {
        return false;
    }

    limit = DefaultPageLimit;
    const auto requestedLimit = request.find("limit");
    if (requestedLimit != request.end()) {
        std::int64_t value = 0;
        if (!readSignedInteger(request, "limit", value) || value <= 0 ||
            value > static_cast<std::int64_t>(MaximumPageLimit)) {
            return false;
        }
        limit = static_cast<std::size_t>(value);
    }
    return true;
}

DurableRequestResult writePage(
    const std::shared_ptr<ClientSession>& session,
    const std::string& operation,
    const std::string& code,
    const std::string& description,
    std::int64_t afterMessageId,
    const MessagePageResult& page) {
    if (page.status() == MessagePageStatus::UserNotFound) {
        return writeFailure(
            session, operation, "USER_NOT_FOUND", "Account not found");
    }

    json response = {
        {"status", "SUCCESS"},
        {"operation", operation},
        {"code", code},
        {"message", description},
        {"messages", json::array()},
        {"through_message_id", page.throughMessageId()},
        {"next_after_message_id", afterMessageId},
        {"has_more", page.hasMore()}
    };
    bool omittedRecord = false;
    try {
        for (const DeliveryMessage& message : page.messages()) {
            json candidate = response;
            candidate["messages"].push_back(deliveryEnvelope(message));
            candidate["next_after_message_id"] = message.message().id();
            candidate["has_more"] = true;
            if (candidate.dump().size() > MaximumResponseBytes) {
                if (response["messages"].empty()) {
                    return writeFailure(
                        session,
                        operation,
                        "RECORD_UNREPRESENTABLE",
                        "Stored message cannot be represented");
                }
                omittedRecord = true;
                break;
            }
            response = std::move(candidate);
        }
        response["has_more"] = page.hasMore() || omittedRecord;
        if (response.dump().size() > MaximumResponseBytes) {
            return writeFailure(
                session,
                operation,
                "RECORD_UNREPRESENTABLE",
                "Stored message cannot be represented");
        }
    } catch (const json::exception&) {
        return writeFailure(
            session,
            operation,
            "RECORD_UNREPRESENTABLE",
            "Stored message cannot be represented");
    } catch (const std::exception&) {
        return writeFailure(
            session,
            operation,
            "RECORD_UNREPRESENTABLE",
            "Stored message cannot be represented");
    }
    return writeResponse(session, response);
}

DurableRequestResult handlePage(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database,
    bool pending) {
    const std::string operation = pending ? "SYNC_PENDING" : "HISTORY";
    std::int64_t afterMessageId = 0;
    std::optional<std::int64_t> throughMessageId;
    std::size_t limit = 0;
    if (!readPageRequest(
            request, afterMessageId, throughMessageId, limit)) {
        return writeFailure(
            session, operation, "INVALID_REQUEST", "Invalid request");
    }
    try {
        const MessagePageResult page = pending
            ? database.getPendingDeliveryPage(
                session->authenticatedUsername(),
                afterMessageId,
                throughMessageId,
                limit)
            : database.getHistoryDeliveryPage(
                session->authenticatedUsername(),
                afterMessageId,
                throughMessageId,
                limit);
        return writePage(
            session,
            operation,
            pending ? "PENDING_PAGE" : "HISTORY_PAGE",
            pending ? "Pending messages" : "Authorized history",
            afterMessageId,
            page);
    } catch (const std::invalid_argument&) {
        return writeFailure(
            session, operation, "INVALID_REQUEST", "Invalid request");
    } catch (const std::runtime_error&) {
        std::cerr << "[DATABASE] Message-page query failed" << std::endl;
        return writeFailure(
            session,
            operation,
            "DATABASE_ERROR",
            "Database operation failed");
    }
}

} // namespace

DurableRequestResult handleDurableRequest(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database,
    SessionRegistry& registry) {
    const auto type = request.find("type");
    if (type == request.end() || !type->is_string()) {
        return DurableRequestResult::NotHandled;
    }
    const std::string operation = type->get<std::string>();
    if (operation == "PRIVATE") {
        return handlePrivate(request, session, database, registry);
    }
    if (operation == "GROUP") {
        return handleGroup(request, session, database, registry);
    }
    if (operation == "BROADCAST") {
        return handleBroadcast(request, session, database, registry);
    }
    if (operation == "DELIVERY_ACK") {
        return handleAcknowledgement(request, session, database);
    }
    if (operation == "SYNC_PENDING") {
        return handlePage(request, session, database, true);
    }
    if (operation == "HISTORY") {
        return handlePage(request, session, database, false);
    }
    return DurableRequestResult::NotHandled;
}
