#include <string>

#include "mod/Mod.hpp"

class HelloMod final : public mc::mod::Mod {
protected:
    int onInit() override {
        on("on_join", [this](const mc::mod::Event& event) {
            broadcast("&eWelcome " + event.asString(0) + " from the C++ mod");
        });

        on("on_server_start", [this](const mc::mod::Event&) {
            info("hello_mod started");
        });

        on("on_server_stop", [this](const mc::mod::Event&) {
            info("hello_mod stopped");
        });

        command("hello", "Say hello from hello_mod", [this](const mc::mod::CommandContext& context) {
            broadcast("&aHello from " + context.sender + " via hello_mod");
        });

        command("modblock", "Place a stone block through hello_mod", [this](const mc::mod::CommandContext& context) {
            if (!context.isOp) {
                broadcast("&c/modblock is operator-only");
                return;
            }
            setBlock(0, 64, 0, 16);
            broadcast("&ahello_mod placed a stone block at 0,64,0");
        });

        return 0;
    }
};

MC_MOD(HelloMod)
