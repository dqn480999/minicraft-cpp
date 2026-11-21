#pragma once

#include <optional>
#include <string>

#include "protocol/Profile.hpp"

namespace mc {

class AuthClient {
public:
    static bool available();
    static std::optional<Profile> hasJoined(const std::string& username, const std::string& serverHash, const std::string& ipAddress);
};

}
