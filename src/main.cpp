#include <iostream>
#include <string>
#include <string_view>

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "server/Server.hpp"

namespace {

void printUsage() {
    std::cout << "usage: minicraft [--config <path>] [--port <port>] [--online] [--offline] [--debug] [--no-color]\n";
}

}

int main(int argc, char** argv) {
    std::string configPath = "config/server.properties";
    bool onlineOverride = false;
    bool offlineOverride = false;
    bool debugOverride = false;
    bool noColorOverride = false;
    std::string portOverride;
    bool help = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--config" || argument == "-c") {
            if (i + 1 >= argc) {
                printUsage();
                return 1;
            }
            configPath = argv[++i];
        } else if (argument == "--port" || argument == "-p") {
            if (i + 1 >= argc) {
                printUsage();
                return 1;
            }
            portOverride = argv[++i];
        } else if (argument == "--online") {
            onlineOverride = true;
            offlineOverride = false;
        } else if (argument == "--offline") {
            offlineOverride = true;
            onlineOverride = false;
        } else if (argument == "--debug") {
            debugOverride = true;
        } else if (argument == "--no-color") {
            noColorOverride = true;
        } else if (argument == "--help" || argument == "-h") {
            help = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            printUsage();
            return 1;
        }
    }

    if (help) {
        printUsage();
        return 0;
    }

    mc::ServerConfig config = mc::Config::load(configPath);
    if (onlineOverride) {
        config.onlineMode = true;
    }
    if (offlineOverride) {
        config.onlineMode = false;
    }
    if (debugOverride) {
        config.debug = true;
    }
    if (noColorOverride) {
        config.color = false;
    }
    if (!portOverride.empty()) {
        mc::Config::applyLine(config, "port=" + portOverride);
    }

    mc::Log::init(config.debug, config.color);
    mc::Log::setFile(config.logFile);

    mc::Server server(config);
    if (!server.start()) {
        mc::Log::error("server failed to start");
        return 1;
    }
    server.run();
    return 0;
}
