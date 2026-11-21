#pragma once

#include <string>
#include <vector>

#include "common/Uuid.hpp"

namespace mc {

struct ProfileProperty {
    std::string name;
    std::string value;
    std::string signature;
};

struct Profile {
    Uuid id;
    std::string name;
    std::vector<ProfileProperty> properties;
};

}
