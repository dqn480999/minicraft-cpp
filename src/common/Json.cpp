#include "common/Json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "common/Types.hpp"

namespace mc {
namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Json parse() {
        skipWhitespace();
        Json value = parseValue();
        skipWhitespace();
        if (position_ != text_.size()) {
            throw ProtocolError("trailing data in json");
        }
        return value;
    }

private:
    std::string_view text_;
    std::size_t position_{0};

    void skipWhitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                ++position_;
            } else {
                break;
            }
        }
    }

    char peek() const {
        if (position_ >= text_.size()) {
            throw ProtocolError("unexpected end of json");
        }
        return text_[position_];
    }

    char take() {
        const char c = peek();
        ++position_;
        return c;
    }

    void expect(char expected) {
        if (take() != expected) {
            throw ProtocolError("invalid json token");
        }
    }

    Json parseValue() {
        const char c = peek();
        switch (c) {
        case 'n':
            consumeLiteral("null");
            return Json{};
        case 't':
            consumeLiteral("true");
            return Json{true};
        case 'f':
            consumeLiteral("false");
            return Json{false};
        case '"':
            return Json{parseString()};
        case '[':
            return parseArray();
        case '{':
            return parseObject();
        default:
            return parseNumber();
        }
    }

    void consumeLiteral(std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) {
            throw ProtocolError("invalid json literal");
        }
        position_ += literal.size();
    }

    Json parseArray() {
        Json result = Json::array();
        expect('[');
        skipWhitespace();
        if (peek() == ']') {
            ++position_;
            return result;
        }
        while (true) {
            skipWhitespace();
            result.push(parseValue());
            skipWhitespace();
            const char c = take();
            if (c == ']') {
                break;
            }
            if (c != ',') {
                throw ProtocolError("invalid json array");
            }
        }
        return result;
    }

    Json parseObject() {
        Json result = Json::object();
        expect('{');
        skipWhitespace();
        if (peek() == '}') {
            ++position_;
            return result;
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                throw ProtocolError("invalid json object key");
            }
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            result[key] = parseValue();
            skipWhitespace();
            const char c = take();
            if (c == '}') {
                break;
            }
            if (c != ',') {
                throw ProtocolError("invalid json object");
            }
        }
        return result;
    }

    static int hexValue(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    unsigned readHex4() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hexValue(take());
            if (digit < 0) {
                throw ProtocolError("invalid unicode escape");
            }
            value = (value << 4) | static_cast<unsigned>(digit);
        }
        return value;
    }

    static void appendUtf8(std::string& out, unsigned code) {
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (true) {
            const char c = take();
            if (c == '"') {
                break;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                throw ProtocolError("control character in json string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            const char escape = take();
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                unsigned code = readHex4();
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (take() != '\\' || take() != 'u') {
                        throw ProtocolError("invalid surrogate pair");
                    }
                    const unsigned low = readHex4();
                    if (low < 0xDC00 || low > 0xDFFF) {
                        throw ProtocolError("invalid surrogate pair");
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, code);
                break;
            }
            default:
                throw ProtocolError("invalid json escape");
            }
        }
        return out;
    }

    Json parseNumber() {
        const std::size_t start = position_;
        if (peek() == '-') {
            ++position_;
        }
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            ++position_;
        }
        bool isDouble = false;
        if (position_ < text_.size() && text_[position_] == '.') {
            isDouble = true;
            ++position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            isDouble = true;
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ == start) {
            throw ProtocolError("invalid json number");
        }
        const std::string token(text_.substr(start, position_ - start));
        if (isDouble) {
            return Json{std::strtod(token.c_str(), nullptr)};
        }
        std::int64_t value = 0;
        const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (ec != std::errc{} || ptr != token.data() + token.size()) {
            throw ProtocolError("invalid json integer");
        }
        return Json{value};
    }
};

}

Json::Json() = default;

Json::Json(std::nullptr_t) : type_(Type::Null) {}

Json::Json(bool value) : type_(Type::Bool), boolValue_(value) {}

Json::Json(int value) : type_(Type::Integer), intValue_(value) {}

Json::Json(std::int64_t value) : type_(Type::Integer), intValue_(value) {}

Json::Json(double value) : type_(Type::Double), doubleValue_(value) {}

Json::Json(const char* value) : type_(Type::String), stringValue_(value == nullptr ? "" : value) {}

Json::Json(std::string value) : type_(Type::String), stringValue_(std::move(value)) {}

Json Json::array() {
    Json value;
    value.type_ = Type::Array;
    return value;
}

Json Json::object() {
    Json value;
    value.type_ = Type::Object;
    return value;
}

Json Json::parse(std::string_view text) {
    return Parser(text).parse();
}

Json::Type Json::type() const {
    return type_;
}

bool Json::isNull() const {
    return type_ == Type::Null;
}

bool Json::isBool() const {
    return type_ == Type::Bool;
}

bool Json::isNumber() const {
    return type_ == Type::Integer || type_ == Type::Double;
}

bool Json::isString() const {
    return type_ == Type::String;
}

bool Json::isArray() const {
    return type_ == Type::Array;
}

bool Json::isObject() const {
    return type_ == Type::Object;
}

bool Json::asBool(bool fallback) const {
    return type_ == Type::Bool ? boolValue_ : fallback;
}

std::int64_t Json::asInt(std::int64_t fallback) const {
    if (type_ == Type::Integer) {
        return intValue_;
    }
    if (type_ == Type::Double) {
        return static_cast<std::int64_t>(doubleValue_);
    }
    return fallback;
}

double Json::asDouble(double fallback) const {
    if (type_ == Type::Double) {
        return doubleValue_;
    }
    if (type_ == Type::Integer) {
        return static_cast<double>(intValue_);
    }
    return fallback;
}

const std::string& Json::asString() const {
    return stringValue_;
}

const Json::Array& Json::asArray() const {
    return arrayValue_;
}

Json::Array& Json::asArray() {
    return arrayValue_;
}

const Json::Object& Json::asObject() const {
    return objectValue_;
}

Json::Object& Json::asObject() {
    return objectValue_;
}

Json& Json::operator[](std::string_view key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        objectValue_.clear();
    }
    return objectValue_[std::string(key)];
}

const Json& Json::operator[](std::string_view key) const {
    static const Json nullValue;
    if (type_ != Type::Object) {
        return nullValue;
    }
    const auto it = objectValue_.find(std::string(key));
    return it == objectValue_.end() ? nullValue : it->second;
}

void Json::push(Json value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        arrayValue_.clear();
    }
    arrayValue_.push_back(std::move(value));
}

bool Json::contains(std::string_view key) const {
    if (type_ != Type::Object) {
        return false;
    }
    return objectValue_.find(std::string(key)) != objectValue_.end();
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

void Json::dumpString(std::string_view value, std::string& out) {
    out.push_back('"');
    for (const char c : value) {
        const auto uc = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (uc < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", uc);
                out += buffer;
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    out.push_back('"');
}

void Json::dumpTo(std::string& out) const {
    switch (type_) {
    case Type::Null:
        out += "null";
        break;
    case Type::Bool:
        out += boolValue_ ? "true" : "false";
        break;
    case Type::Integer:
        out += std::to_string(intValue_);
        break;
    case Type::Double: {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", doubleValue_);
        out += buffer;
        break;
    }
    case Type::String:
        dumpString(stringValue_, out);
        break;
    case Type::Array: {
        out.push_back('[');
        bool first = true;
        for (const auto& item : arrayValue_) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            item.dumpTo(out);
        }
        out.push_back(']');
        break;
    }
    case Type::Object: {
        out.push_back('{');
        bool first = true;
        for (const auto& [key, value] : objectValue_) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            dumpString(key, out);
            out.push_back(':');
            value.dumpTo(out);
        }
        out.push_back('}');
        break;
    }
    }
}

}
