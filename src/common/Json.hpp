#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mc {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    enum class Type {
        Null,
        Bool,
        Integer,
        Double,
        String,
        Array,
        Object
    };

    Json();
    Json(std::nullptr_t);
    Json(bool value);
    Json(int value);
    Json(std::int64_t value);
    Json(double value);
    Json(const char* value);
    Json(std::string value);

    static Json array();
    static Json object();
    static Json parse(std::string_view text);

    Type type() const;
    bool isNull() const;
    bool isBool() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;

    bool asBool(bool fallback = false) const;
    std::int64_t asInt(std::int64_t fallback = 0) const;
    double asDouble(double fallback = 0.0) const;
    const std::string& asString() const;

    const Array& asArray() const;
    Array& asArray();
    const Object& asObject() const;
    Object& asObject();

    Json& operator[](std::string_view key);
    const Json& operator[](std::string_view key) const;

    void push(Json value);
    bool contains(std::string_view key) const;

    std::string dump() const;
    void dumpTo(std::string& out) const;

private:
    Type type_{Type::Null};
    bool boolValue_{false};
    std::int64_t intValue_{0};
    double doubleValue_{0.0};
    std::string stringValue_;
    Array arrayValue_;
    Object objectValue_;

    static void dumpString(std::string_view value, std::string& out);
};

}
