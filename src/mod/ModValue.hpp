#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace mc {

struct ModValue {
    bool isString{false};
    std::string text;
    std::int64_t number{0};

    static ModValue string(std::string value) {
        ModValue result;
        result.isString = true;
        result.text = std::move(value);
        return result;
    }

    static ModValue integer(std::int64_t value) {
        ModValue result;
        result.number = value;
        return result;
    }
};

}
