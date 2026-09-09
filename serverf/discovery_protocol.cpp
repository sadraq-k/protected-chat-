#include "discovery_protocol.h"

#include "client_session.h"
#include "../database.h"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr std::size_t DefaultPageLimit = 50;
constexpr std::size_t MaximumPageLimit = 100;
constexpr std::size_t MaximumResponseBytes = 1'048'576;

DiscoveryRequestResult writeResponse(
    const std::shared_ptr<ClientSession>& session,
    const json& response) {
    return session->sendJson(response) == SendResult::Written
        ? DiscoveryRequestResult::Handled
        : DiscoveryRequestResult::Stop;
}

DiscoveryRequestResult writeFailure(
    const std::shared_ptr<ClientSession>& session,
    const std::string& operation,
    const std::string& code,
    const std::string& message) {
    return writeResponse(session, {
        {"status", "FAIL"},
        {"operation", operation},
        {"code", code},
        {"message", message}
    });
}

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

bool readPageArguments(
    const json& request,
    const char* cursorField,
    std::int64_t& cursor,
    std::size_t& limit) {
    cursor = 0;
    const auto requestedCursor = request.find(cursorField);
    if (requestedCursor != request.end() &&
        (!readSignedInteger(request, cursorField, cursor) || cursor < 0)) {
        return false;
    }

    limit = DefaultPageLimit;
    const auto requestedLimit = request.find("limit");
    if (requestedLimit != request.end()) {
        std::int64_t parsedLimit = 0;
        if (!readSignedInteger(request, "limit", parsedLimit) ||
            parsedLimit < 1 ||
            parsedLimit > static_cast<std::int64_t>(MaximumPageLimit)) {
            return false;
        }
        limit = static_cast<std::size_t>(parsedLimit);
    }
    return true;
}

bool responseFits(const json& response) {
    return response.dump().size() <= MaximumResponseBytes;
}

DiscoveryRequestResult processContactAdd(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database) {
    const std::string operation = "CONTACT_ADD";
    const auto username = request.find("username");
    if (!containsOnlyFields(request, {"type", "username"}) ||
        username == request.end() || !username->is_string()) {
        return writeFailure(
            session, operation, "INVALID_REQUEST", "Invalid request");
    }

    try {
        const ContactAddResult result = database.addContact(
            session->authenticatedUsername(), username->get<std::string>());
        if (result.status() == ContactAddStatus::OwnerNotFound) {
            return writeFailure(
                session,
                operation,
                "ACTOR_NOT_FOUND",
                "Authenticated user is unavailable");
        }
        if (result.status() == ContactAddStatus::ContactNotFound) {
            return writeFailure(
                session,
                operation,
                "CONTACT_NOT_FOUND",
                "Contact user does not exist");
        }
        if (result.status() == ContactAddStatus::SelfContactNotAllowed) {
            return writeFailure(
                session,
                operation,
                "SELF_CONTACT_NOT_ALLOWED",
                "You cannot add yourself as a contact");
        }
        if (!result.contact()) {
            throw std::runtime_error("Contact result is missing its identity");
        }

        const bool added = result.status() == ContactAddStatus::Added;
        const UserSummary& contact = *result.contact();
        const json response = {
            {"status", "SUCCESS"},
            {"operation", operation},
            {"code", added ? "ADDED" : "ALREADY_CONTACT"},
            {"message", added
                ? "Contact added" : "User is already a contact"},
            {"contact", {
                {"user_id", contact.id()},
                {"username", contact.username()}
            }}
        };
        if (!responseFits(response)) {
            return writeFailure(
                session,
                operation,
                "RECORD_UNREPRESENTABLE",
                "Stored record cannot be represented within response limits");
        }
        return writeResponse(session, response);
    } catch (const std::invalid_argument&) {
        return writeFailure(
            session, operation, "INVALID_REQUEST", "Invalid request");
    } catch (const json::exception&) {
        return writeFailure(
            session,
            operation,
            "RECORD_UNREPRESENTABLE",
            "Stored record cannot be represented within response limits");
    } catch (const std::runtime_error& error) {
        std::cerr << "[DISCOVERY] Contact-add database failure: "
                  << error.what() << std::endl;
        return writeFailure(
            session, operation, "DATABASE_ERROR", "Database operation failed");
    }
}

DiscoveryRequestResult processContactList(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database) {
    const std::string operation = "CONTACT_LIST";
    std::int64_t afterUserId = 0;
    std::size_t limit = DefaultPageLimit;
    if (!containsOnlyFields(
            request, {"type", "after_user_id", "limit"}) ||
        !readPageArguments(
            request, "after_user_id", afterUserId, limit)) {
        return writeFailure(
            session, operation, "INVALID_REQUEST", "Invalid request");
    }

    try {
        const ContactListResult result = database.listContacts(
            session->authenticatedUsername(), afterUserId, limit);
        if (result.status() == DiscoveryReadStatus::ActorNotFound) {
            return writeFailure(
                session,
                operation,
                "ACTOR_NOT_FOUND",
                "Authenticated user is unavailable");
        }

        json response = {
            {"status", "SUCCESS"},
            {"operation", operation},
            {"code", "CONTACTS_PAGE"},
            {"message", "Contacts"},
            {"contacts", json::array()},
            {"next_after_user_id", afterUserId},
            {"has_more", result.hasMore()}
        };
        const std::vector<UserSummary>& contacts = result.contacts();
        for (std::size_t index = 0; index < contacts.size(); ++index) {
            const UserSummary& contact = contacts[index];
            json candidate = response;
            candidate["contacts"].push_back({
                {"user_id", contact.id()},
                {"username", contact.username()}
            });
            candidate["next_after_user_id"] = contact.id();
            candidate["has_more"] =
                index + 1 < contacts.size() || result.hasMore();
            if (!responseFits(candidate)) {
                if (response["contacts"].empty()) {
                    return writeFailure(
                        session,
                        operation,
                        "RECORD_UNREPRESENTABLE",
                        "Stored record cannot be represented within response limits");
                }
                response["has_more"] = true;
                break;
            }
            response = std::move(candidate);
        }
        return writeResponse(session, response);
    } catch (const std::invalid_argument&) {
        return writeFailure(
            session, operation, "INVALID_REQUEST", "Invalid request");
    } catch (const json::exception&) {
        return writeFailure(
            session,
            operation,
            "RECORD_UNREPRESENTABLE",
            "Stored record cannot be represented within response limits");
    } catch (const std::runtime_error& error) {
        std::cerr << "[DISCOVERY] Contact-list database failure: "
                  << error.what() << std::endl;
        return writeFailure(
            session, operation, "DATABASE_ERROR", "Database operation failed");
    }
}

DiscoveryRequestResult processUserSearch(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database) {
    const std::string operation = "USER_SEARCH";
    const auto query = request.find("query");
    std::int64_t afterUserId = 0;
    std::size_t limit = DefaultPageLimit;
    if (!containsOnlyFields(
            request, {"type", "query", "after_user_id", "limit"}) ||
        query == request.end() || !query->is_string() ||
        !readPageArguments(
            request, "after_user_id", afterUserId, limit)) {
        return writeFailure(
            session, operation, "INVALID_REQUEST", "Invalid request");
    }

    try {
        const std::string queryText = query->get<std::string>();
        const UserSearchResult result = database.searchUsers(
            session->authenticatedUsername(), queryText, afterUserId, limit);
        if (result.status() == DiscoveryReadStatus::ActorNotFound) {
            return writeFailure(
                session,
                operation,
                "ACTOR_NOT_FOUND",
                "Authenticated user is unavailable");
        }

        json response = {
            {"status", "SUCCESS"},
            {"operation", operation},
            {"code", "USERS_PAGE"},
            {"message", "User search results"},
            {"query", queryText},
            {"users", json::array()},
            {"next_after_user_id", afterUserId},
            {"has_more", result.hasMore()}
        };
        const std::vector<UserSearchEntry>& users = result.users();
        for (std::size_t index = 0; index < users.size(); ++index) {
            const UserSearchEntry& entry = users[index];
            json candidate = response;
            candidate["users"].push_back({
                {"user_id", entry.user().id()},
                {"username", entry.user().username()},
                {"is_contact", entry.isContact()}
            });
            candidate["next_after_user_id"] = entry.user().id();
            candidate["has_more"] =
                index + 1 < users.size() || result.hasMore();
            if (!responseFits(candidate)) {
                if (response["users"].empty()) {
                    return writeFailure(
                        session,
                        operation,
                        "RECORD_UNREPRESENTABLE",
                        "Stored record cannot be represented within response limits");
                }
                response["has_more"] = true;
                break;
            }
            response = std::move(candidate);
        }
        return writeResponse(session, response);
    } catch (const std::invalid_argument&) {
        return writeFailure(
            session, operation, "INVALID_REQUEST", "Invalid request");
    } catch (const json::exception&) {
        return writeFailure(
            session,
            operation,
            "RECORD_UNREPRESENTABLE",
            "Stored record cannot be represented within response limits");
    } catch (const std::runtime_error& error) {
        std::cerr << "[DISCOVERY] User-search database failure: "
                  << error.what() << std::endl;
        return writeFailure(
            session, operation, "DATABASE_ERROR", "Database operation failed");
    }
}

DiscoveryRequestResult processGroupSearch(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database) {
    const std::string operation = "GROUP_SEARCH";
    const auto query = request.find("query");
    std::int64_t afterGroupId = 0;
    std::size_t limit = DefaultPageLimit;
    if (!containsOnlyFields(
            request, {"type", "query", "after_group_id", "limit"}) ||
        query == request.end() || !query->is_string() ||
        !readPageArguments(
            request, "after_group_id", afterGroupId, limit)) {
        return writeFailure(
            session, operation, "INVALID_REQUEST", "Invalid request");
    }

    try {
        const std::string queryText = query->get<std::string>();
        const GroupSearchResult result = database.searchGroups(
            session->authenticatedUsername(), queryText, afterGroupId, limit);
        if (result.status() == DiscoveryReadStatus::ActorNotFound) {
            return writeFailure(
                session,
                operation,
                "ACTOR_NOT_FOUND",
                "Authenticated user is unavailable");
        }

        json response = {
            {"status", "SUCCESS"},
            {"operation", operation},
            {"code", "GROUPS_PAGE"},
            {"message", "Group search results"},
            {"query", queryText},
            {"groups", json::array()},
            {"next_after_group_id", afterGroupId},
            {"has_more", result.hasMore()}
        };
        const std::vector<GroupSearchEntry>& groups = result.groups();
        for (std::size_t index = 0; index < groups.size(); ++index) {
            const GroupSearchEntry& entry = groups[index];
            json candidate = response;
            candidate["groups"].push_back({
                {"group_id", entry.group().id()},
                {"name", entry.group().name()},
                {"is_member", entry.isMember()}
            });
            candidate["next_after_group_id"] = entry.group().id();
            candidate["has_more"] =
                index + 1 < groups.size() || result.hasMore();
            if (!responseFits(candidate)) {
                if (response["groups"].empty()) {
                    return writeFailure(
                        session,
                        operation,
                        "RECORD_UNREPRESENTABLE",
                        "Stored record cannot be represented within response limits");
                }
                response["has_more"] = true;
                break;
            }
            response = std::move(candidate);
        }
        return writeResponse(session, response);
    } catch (const std::invalid_argument&) {
        return writeFailure(
            session, operation, "INVALID_REQUEST", "Invalid request");
    } catch (const json::exception&) {
        return writeFailure(
            session,
            operation,
            "RECORD_UNREPRESENTABLE",
            "Stored record cannot be represented within response limits");
    } catch (const std::runtime_error& error) {
        std::cerr << "[DISCOVERY] Group-search database failure: "
                  << error.what() << std::endl;
        return writeFailure(
            session, operation, "DATABASE_ERROR", "Database operation failed");
    }
}

} // namespace

DiscoveryRequestResult handleDiscoveryRequest(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database) {
    if (!request.is_object()) {
        return DiscoveryRequestResult::NotHandled;
    }
    const auto type = request.find("type");
    if (type == request.end() || !type->is_string()) {
        return DiscoveryRequestResult::NotHandled;
    }
    const std::string operation = type->get<std::string>();
    if (operation == "CONTACT_ADD") {
        return processContactAdd(request, session, database);
    }
    if (operation == "CONTACT_LIST") {
        return processContactList(request, session, database);
    }
    if (operation == "USER_SEARCH") {
        return processUserSearch(request, session, database);
    }
    if (operation == "GROUP_SEARCH") {
        return processGroupSearch(request, session, database);
    }
    return DiscoveryRequestResult::NotHandled;
}
