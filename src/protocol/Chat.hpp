#pragma once

#include <string>

namespace mc::chat {

std::string text(const std::string& message);
std::string colored(const std::string& message, const std::string& color);
std::string legacy(const std::string& message);
std::string sanitize(const std::string& message);
std::string plain(const std::string& message);
std::string section(const std::string& message);
std::string playerMessage(const std::string& name, const std::string& message);
std::string statusDescription(const std::string& motd);

}
