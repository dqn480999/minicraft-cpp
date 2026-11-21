#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/Types.hpp"

namespace mc {

class Connection;

class PlayerManager {
public:
    bool add(const std::shared_ptr<Connection>& connection, std::size_t maxPlayers);
    bool remove(std::int32_t entityId);
    std::vector<std::shared_ptr<Connection>> snapshot() const;
    std::shared_ptr<Connection> byName(std::string_view name) const;
    bool nameTaken(std::string_view name) const;
    std::size_t count() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::int32_t, std::weak_ptr<Connection>> players_;
};

}
