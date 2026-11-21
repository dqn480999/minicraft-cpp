#include "server/PlayerManager.hpp"

#include <algorithm>
#include <cctype>

#include "net/Connection.hpp"

namespace mc {
namespace {

bool equalsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto a = static_cast<unsigned char>(left[i]);
        const auto b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

}

bool PlayerManager::add(const std::shared_ptr<Connection>& connection, std::size_t maxPlayers) {
    std::lock_guard lock(mutex_);
    if (players_.size() >= maxPlayers || players_.contains(connection->entityId())) {
        return false;
    }
    for (const auto& [entityId, weak] : players_) {
        if (const auto existing = weak.lock(); existing && equalsIgnoreCase(existing->username(), connection->username())) {
            return false;
        }
    }
    players_[connection->entityId()] = connection;
    return true;
}

bool PlayerManager::remove(std::int32_t entityId) {
    std::lock_guard lock(mutex_);
    return players_.erase(entityId) > 0;
}

std::vector<std::shared_ptr<Connection>> PlayerManager::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Connection>> result;
    result.reserve(players_.size());
    for (const auto& [entityId, weak] : players_) {
        if (const auto connection = weak.lock()) {
            result.push_back(connection);
        }
    }
    return result;
}

std::shared_ptr<Connection> PlayerManager::byName(std::string_view name) const {
    std::lock_guard lock(mutex_);
    for (const auto& [entityId, weak] : players_) {
        if (const auto connection = weak.lock(); connection && equalsIgnoreCase(connection->username(), name)) {
            return connection;
        }
    }
    return nullptr;
}

bool PlayerManager::nameTaken(std::string_view name) const {
    return byName(name) != nullptr;
}

std::size_t PlayerManager::count() const {
    std::lock_guard lock(mutex_);
    return players_.size();
}

void PlayerManager::clear() {
    std::lock_guard lock(mutex_);
    players_.clear();
}

}
