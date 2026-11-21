#include "protocol/Auth.hpp"

#include <mutex>

#include "common/Json.hpp"
#include "common/Log.hpp"
#include "common/Uuid.hpp"

#ifdef MINICRAFT_HAVE_CURL
#include <curl/curl.h>
#endif

namespace mc {
namespace {

std::string urlEncode(const std::string& value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size() * 3);
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = (byte >= 'A' && byte <= 'Z')
            || (byte >= 'a' && byte <= 'z')
            || (byte >= '0' && byte <= '9')
            || byte == '-' || byte == '_' || byte == '.' || byte == '~';
        if (unreserved) {
            output.push_back(c);
        } else {
            output.push_back('%');
            output.push_back(digits[byte >> 4]);
            output.push_back(digits[byte & 0x0F]);
        }
    }
    return output;
}

std::string stripPort(const std::string& address) {
    if (address.empty()) {
        return address;
    }
    if (address.front() == '[') {
        const auto end = address.find(']');
        if (end != std::string::npos) {
            return address.substr(1, end - 1);
        }
    }
    const auto first = address.find(':');
    if (first != std::string::npos && address.find(':', first + 1) == std::string::npos) {
        return address.substr(0, first);
    }
    return address;
}

#ifdef MINICRAFT_HAVE_CURL
std::once_flag g_curlInit;

void ensureCurl() {
    std::call_once(g_curlInit, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

std::size_t writeCallback(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto* output = static_cast<std::string*>(userdata);
    output->append(data, size * count);
    return size * count;
}
#endif

}

bool AuthClient::available() {
#ifdef MINICRAFT_HAVE_CURL
    return true;
#else
    return false;
#endif
}

std::optional<Profile> AuthClient::hasJoined(const std::string& username, const std::string& serverHash, const std::string& ipAddress) {
#ifdef MINICRAFT_HAVE_CURL
    ensureCurl();

    std::string url = "https://sessionserver.mojang.com/session/minecraft/hasJoined?username="
        + urlEncode(username) + "&serverId=" + urlEncode(serverHash);
    if (!ipAddress.empty()) {
        url += "&ip=" + urlEncode(stripPort(ipAddress));
    }

    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        Log::error("failed to initialize curl");
        return std::nullopt;
    }

    std::string response;
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "minicraft-cpp/0.1");

    const CURLcode result = curl_easy_perform(handle);
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(handle);

    if (result != CURLE_OK) {
        Log::warn("session server request failed: {}", curl_easy_strerror(result));
        return std::nullopt;
    }
    if (status != 200 || response.empty()) {
        Log::info("session server rejected {} with status {}", username, status);
        return std::nullopt;
    }

    try {
        const Json root = Json::parse(response);
        if (!root.isObject() || !root.contains("id")) {
            return std::nullopt;
        }
        Profile profile;
        profile.id = Uuid::fromString(root["id"].asString());
        profile.name = root.contains("name") ? root["name"].asString() : username;
        if (root.contains("properties") && root["properties"].isArray()) {
            for (const auto& property : root["properties"].asArray()) {
                if (!property.isObject()) {
                    continue;
                }
                ProfileProperty entry;
                entry.name = property["name"].asString();
                entry.value = property["value"].asString();
                if (property.contains("signature")) {
                    entry.signature = property["signature"].asString();
                }
                profile.properties.push_back(std::move(entry));
            }
        }
        return profile;
    } catch (const std::exception& error) {
        Log::warn("failed to parse session response: {}", error.what());
        return std::nullopt;
    }
#else
    (void)username;
    (void)serverHash;
    (void)ipAddress;
    Log::error("online-mode requested but libcurl support is unavailable");
    return std::nullopt;
#endif
}

}
