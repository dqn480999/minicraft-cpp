#include "protocol/Chat.hpp"

#include <array>
#include <cctype>

#include "common/Json.hpp"

namespace mc::chat {

std::string text(const std::string& message) {
    Json root = Json::object();
    root["text"] = message;
    return root.dump();
}

std::string colored(const std::string& message, const std::string& color) {
    Json root = Json::object();
    root["text"] = message;
    root["color"] = color;
    return root.dump();
}

namespace {

struct Style {
    std::string color;
    bool bold{false};
    bool italic{false};
    bool underline{false};
    bool strikethrough{false};
    bool obfuscated{false};
};

bool isCode(char c) {
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return (lower >= '0' && lower <= '9') || (lower >= 'a' && lower <= 'f')
        || lower == 'k' || lower == 'l' || lower == 'm' || lower == 'n' || lower == 'o' || lower == 'r';
}

bool applyCode(char c, Style& style) {
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static constexpr std::array<const char*, 16> colors = {
        "black", "dark_blue", "dark_green", "dark_aqua",
        "dark_red", "dark_purple", "gold", "gray",
        "dark_gray", "blue", "green", "aqua",
        "red", "light_purple", "yellow", "white"
    };
    if (lower >= '0' && lower <= '9') {
        style.color = colors[static_cast<std::size_t>(lower - '0')];
        return true;
    }
    if (lower >= 'a' && lower <= 'f') {
        style.color = colors[static_cast<std::size_t>(lower - 'a' + 10)];
        return true;
    }
    if (lower == 'r') {
        style = Style{};
        return true;
    }
    if (lower == 'k') {
        style.obfuscated = true;
        return true;
    }
    if (lower == 'l') {
        style.bold = true;
        return true;
    }
    if (lower == 'm') {
        style.strikethrough = true;
        return true;
    }
    if (lower == 'n') {
        style.underline = true;
        return true;
    }
    if (lower == 'o') {
        style.italic = true;
        return true;
    }
    return false;
}

Json styleComponent(const std::string& text, const Style& style) {
    Json component = Json::object();
    component["text"] = text;
    if (!style.color.empty()) {
        component["color"] = style.color;
    }
    if (style.bold) {
        component["bold"] = true;
    }
    if (style.italic) {
        component["italic"] = true;
    }
    if (style.underline) {
        component["underlined"] = true;
    }
    if (style.strikethrough) {
        component["strikethrough"] = true;
    }
    if (style.obfuscated) {
        component["obfuscated"] = true;
    }
    return component;
}

void appendComponents(const std::string& message, bool ampersand, Json& root) {
    Style style;
    std::string buffer;
    Json extra = Json::array();

    const auto flush = [&] {
        if (buffer.empty()) {
            return;
        }
        extra.push(styleComponent(buffer, style));
        buffer.clear();
    };

    for (std::size_t i = 0; i < message.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(message[i]);
        char code = 0;
        bool marker = false;
        if (byte == 0xC2 && i + 2 < message.size() && static_cast<unsigned char>(message[i + 1]) == 0xA7) {
            code = message[i + 2];
            marker = isCode(code);
            if (marker) {
                i += 2;
            }
        } else if (ampersand && byte == '&' && i + 1 < message.size() && isCode(message[i + 1])) {
            code = message[i + 1];
            marker = true;
            ++i;
        }
        if (marker) {
            flush();
            applyCode(code, style);
            continue;
        }
        buffer.push_back(message[i]);
    }
    flush();

    if (extra.asArray().empty()) {
        root["text"] = message;
    } else {
        root["text"] = "";
        root["extra"] = std::move(extra);
    }
}

}

std::string legacy(const std::string& message) {
    Json root = Json::object();
    appendComponents(message, true, root);
    return root.dump();
}

std::string sanitize(const std::string& message) {
    std::string output;
    output.reserve(message.size());
    for (std::size_t i = 0; i < message.size(); ++i) {
        const auto byte = static_cast<unsigned char>(message[i]);
        if (byte == 0xC2 && i + 2 < message.size() && static_cast<unsigned char>(message[i + 1]) == 0xA7) {
            i += 2;
            continue;
        }
        if (byte < 0x20 && byte != '\t') {
            continue;
        }
        output.push_back(message[i]);
    }
    return output;
}

std::string plain(const std::string& message) {
    std::string output;
    output.reserve(message.size());
    for (std::size_t i = 0; i < message.size(); ++i) {
        const auto byte = static_cast<unsigned char>(message[i]);
        if (byte == 0xC2 && i + 2 < message.size() && static_cast<unsigned char>(message[i + 1]) == 0xA7
            && isCode(message[i + 2])) {
            i += 2;
            continue;
        }
        if (byte == '&' && i + 1 < message.size() && isCode(message[i + 1])) {
            ++i;
            continue;
        }
        output.push_back(message[i]);
    }
    return output;
}

std::string section(const std::string& message) {
    std::string output;
    output.reserve(message.size());
    for (std::size_t i = 0; i < message.size(); ++i) {
        const auto byte = static_cast<unsigned char>(message[i]);
        if (byte == '&' && i + 1 < message.size() && isCode(message[i + 1])) {
            output += "\xC2\xA7";
            output.push_back(message[i + 1]);
            ++i;
            continue;
        }
        if (byte == 0xC2 && i + 2 < message.size() && static_cast<unsigned char>(message[i + 1]) == 0xA7) {
            output.append(message, i, 3);
            i += 2;
            continue;
        }
        output.push_back(message[i]);
    }
    return output;
}

std::string playerMessage(const std::string& name, const std::string& message) {
    Json root = Json::object();
    root["text"] = "";
    Json extra = Json::array();
    Json prefix = Json::object();
    prefix["text"] = "<";
    prefix["color"] = "gray";
    extra.push(std::move(prefix));
    Json nameComponent = Json::object();
    nameComponent["text"] = name;
    nameComponent["color"] = "white";
    extra.push(std::move(nameComponent));
    Json separator = Json::object();
    separator["text"] = "> ";
    separator["color"] = "gray";
    extra.push(std::move(separator));
    root["extra"] = std::move(extra);

    Json messageRoot = Json::object();
    appendComponents(sanitize(message), true, messageRoot);
    if (messageRoot.contains("extra")) {
        for (const auto& part : messageRoot["extra"].asArray()) {
            root["extra"].push(part);
        }
    } else {
        root["extra"].push(messageRoot);
    }
    return root.dump();
}

std::string statusDescription(const std::string& motd) {
    return legacy(motd);
}

}
