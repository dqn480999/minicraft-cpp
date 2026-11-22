#include "server/CommandManager.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "common/Log.hpp"
#include "net/Connection.hpp"
#include "protocol/Chat.hpp"
#include "protocol/ItemStack.hpp"
#include "server/Server.hpp"

namespace mc {
namespace {

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::vector<std::string> split(std::string_view text) {
    std::vector<std::string> tokens;
    std::istringstream stream{std::string(text)};
    std::string token;
    while (stream >> token) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

std::string join(const std::vector<std::string>& tokens, std::size_t start) {
    std::string result;
    for (std::size_t i = start; i < tokens.size(); ++i) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += tokens[i];
    }
    return result;
}

std::optional<int> parseInt(std::string_view text) {
    int value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parseDouble(std::string_view text) {
    double value = 0.0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<int> parseGameMode(const std::string& value) {
    if (const auto parsed = parseInt(value)) {
        return parsed;
    }
    const auto name = lower(value);
    if (name == "survival" || name == "s") {
        return 0;
    }
    if (name == "creative" || name == "c") {
        return 1;
    }
    if (name == "adventure" || name == "a") {
        return 2;
    }
    if (name == "spectator" || name == "sp") {
        return 3;
    }
    return std::nullopt;
}

}

CommandManager::CommandManager(Server& server) : server_(server) {}

void CommandManager::execute(const std::shared_ptr<Connection>& source, std::string_view line) {
    std::string text = trim(line);
    if (!text.empty() && text.front() == '/') {
        text.erase(text.begin());
    }
    if (text.empty()) {
        return;
    }

    const auto tokens = split(text);
    if (tokens.empty()) {
        return;
    }
    const std::string command = lower(tokens.front());
    const bool administrative = command == "say" || command == "gamemode" || command == "gm"
        || command == "tp" || command == "teleport" || command == "time" || command == "kick"
        || command == "stop" || command == "op" || command == "deop" || command == "save" || command == "cleanup" || command == "clearitems"
        || command == "give" || command == "clear" || command == "setblock"
        || command == "heal" || command == "feed" || command == "kill" || command == "summon"
        || command == "setfood" || command == "script";
    if (administrative && !requireOp(source)) {
        return;
    }

    server_.scripts().fire("on_command",
                           {source ? source->entityId() : 0},
                           {command, join(tokens, 1)});
    server_.mods().fire("on_command",
                        {ModValue::string(command), ModValue::string(join(tokens, 1)),
                         ModValue::integer(source ? source->entityId() : 0)});

    if (command == "help") {
        help(source);
    } else if (command == "list") {
        list(source);
    } else if (command == "say") {
        say(source, join(tokens, 1));
    } else if (command == "me") {
        me(source, join(tokens, 1));
    } else if (command == "gamemode" || command == "gm") {
        gameMode(source, tokens);
    } else if (command == "tp" || command == "teleport") {
        teleport(source, tokens);
    } else if (command == "time") {
        time(source, tokens);
    } else if (command == "seed") {
        seed(source);
    } else if (command == "kick") {
        kick(source, tokens);
    } else if (command == "spawn") {
        spawn(source);
    } else if (command == "stop") {
        stop(source);
    } else if (command == "op") {
        opCommand(source, tokens, true);
    } else if (command == "deop") {
        opCommand(source, tokens, false);
    } else if (command == "info" || command == "tps") {
        info(source);
    } else if (command == "save") {
        save(source);
    } else if (command == "cleanup" || command == "clearitems") {
        cleanup(source);
    } else if (command == "give") {
        give(source, tokens);
    } else if (command == "clear") {
        clear(source, tokens);
    } else if (command == "setblock") {
        setBlock(source, tokens);
    } else if (command == "heal") {
        heal(source, tokens);
    } else if (command == "feed") {
        feed(source, tokens);
    } else if (command == "kill") {
        kill(source, tokens);
    } else if (command == "summon") {
        summon(source, tokens);
    } else if (command == "setfood") {
        setFood(source, tokens);
    } else if (command == "script") {
        script(source, tokens);
    } else if (command == "mods") {
        modsList(source);
    } else if (server_.mods().executeCommand(source ? source->username() : "console",
                                             source ? source->entityId() : 0,
                                             source ? server_.isOp(source->username()) : true,
                                             command,
                                             join(tokens, 1))) {
        return;
    } else {
        feedback(source, "&cUnknown command: &f" + command);
    }
}

void CommandManager::help(const std::shared_ptr<Connection>& source) {
    feedback(source, "&eCommands: &f/help /list /info /say /me /gamemode /tp /time /seed /kick /spawn /stop /op /deop /save /cleanup /give /clear /setblock /heal /feed /kill /summon /setfood /script /mods");
}

void CommandManager::list(const std::shared_ptr<Connection>& source) {
    const auto players = server_.players().snapshot();
    std::string names;
    for (const auto& player : players) {
        if (!names.empty()) {
            names += ", ";
        }
        names += player->username();
    }
    feedback(source, "&aOnline (&f" + std::to_string(players.size()) + "&a): &f" + names);
}

void CommandManager::say(const std::shared_ptr<Connection>& source, std::string_view message) {
    if (message.empty()) {
        feedback(source, "&cUsage: /say <message>");
        return;
    }
    server_.broadcastSystem("&e[Server] &f" + std::string(message));
}

void CommandManager::me(const std::shared_ptr<Connection>& source, std::string_view message) {
    if (!source || message.empty()) {
        feedback(source, "&cUsage: /me <action>");
        return;
    }
    server_.broadcastChat("&d* " + source->username() + " &f" + std::string(message));
}

void CommandManager::gameMode(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        feedback(source, "&cUsage: /gamemode <mode> [player]");
        return;
    }
    const auto mode = parseGameMode(arguments[1]);
    if (!mode) {
        feedback(source, "&cInvalid game mode: &f" + arguments[1]);
        return;
    }

    std::shared_ptr<Connection> target;
    if (arguments.size() >= 3) {
        target = server_.players().byName(arguments[2]);
        if (!target) {
            feedback(source, "&cPlayer not found: &f" + arguments[2]);
            return;
        }
    } else if (source) {
        target = source;
    } else {
        feedback(source, "&cConsole must specify a player");
        return;
    }

    server_.setGameMode(target, *mode);
    feedback(source, "&a" + target->username() + " is now in game mode " + std::to_string(std::clamp(*mode, 0, 3)));
}

void CommandManager::teleport(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    if (!source) {
        feedback(source, "&cConsole cannot teleport itself");
        return;
    }
    if (arguments.size() < 4) {
        feedback(source, "&cUsage: /tp <x> <y> <z>");
        return;
    }
    const auto x = parseDouble(arguments[1]);
    const auto y = parseDouble(arguments[2]);
    const auto z = parseDouble(arguments[3]);
    if (!x || !y || !z) {
        feedback(source, "&cInvalid coordinates");
        return;
    }
    server_.teleport(source, *x, *y, *z);
    feedback(source, "&aTeleported");
}

void CommandManager::time(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    std::size_t index = 1;
    if (arguments.size() > 1 && lower(arguments[1]) == "set") {
        index = 2;
    }
    if (arguments.size() <= index) {
        feedback(source, "&eTime of day: &f" + std::to_string(server_.world().timeOfDay()));
        return;
    }
    const auto value = parseInt(arguments[index]);
    if (!value) {
        feedback(source, "&cInvalid time value");
        return;
    }
    server_.world().setTimeOfDay(*value);
    server_.broadcastTime();
    feedback(source, "&aTime set to &f" + std::to_string(*value));
}

void CommandManager::seed(const std::shared_ptr<Connection>& source) {
    feedback(source, "&eWorld seed: &f" + std::to_string(server_.world().seed()));
}

void CommandManager::kick(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        feedback(source, "&cUsage: /kick <player> [reason]");
        return;
    }
    const auto target = server_.players().byName(arguments[1]);
    if (!target) {
        feedback(source, "&cPlayer not found: &f" + arguments[1]);
        return;
    }
    std::string reason = arguments.size() > 2 ? join(arguments, 2) : "Kicked by an operator";
    server_.kick(target, reason);
    feedback(source, "&aKicked &f" + target->username());
}

void CommandManager::spawn(const std::shared_ptr<Connection>& source) {
    if (!source) {
        feedback(source, "&cConsole cannot teleport itself");
        return;
    }
    server_.teleport(source,
                     static_cast<double>(server_.world().spawnX()) + 0.5,
                     static_cast<double>(server_.world().spawnY()),
                     static_cast<double>(server_.world().spawnZ()) + 0.5);
    feedback(source, "&aTeleported to spawn");
}

void CommandManager::stop(const std::shared_ptr<Connection>& source) {
    feedback(source, "&cStopping server");
    server_.requestStop();
}

void CommandManager::opCommand(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments, bool value) {
    if (arguments.size() < 2) {
        feedback(source, value ? "&cUsage: /op <player>" : "&cUsage: /deop <player>");
        return;
    }
    server_.setOp(arguments[1], value);
    feedback(source, (value ? "&aOpped &f" : "&aDe-opped &f") + arguments[1]);
}

void CommandManager::info(const std::shared_ptr<Connection>& source) {
    const auto seconds = server_.uptimeSeconds();
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto remainder = seconds % 60;
    feedback(source, "&aOnline &f" + std::to_string(server_.players().count()) + "/"
        + std::to_string(server_.config().maxPlayers)
        + ", chunks " + std::to_string(server_.world().loadedChunks())
        + ", uptime " + std::to_string(hours) + "h "
        + std::to_string(minutes) + "m "
        + std::to_string(remainder) + "s");
}

void CommandManager::save(const std::shared_ptr<Connection>& source) {
    server_.saveAllPlayers();
    server_.world().save();
    feedback(source, "&aWorld and player data saved");
}

void CommandManager::cleanup(const std::shared_ptr<Connection>& source) {
    server_.cleanupItems();
    feedback(source, "&aDropped items cleared");
}

void CommandManager::give(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    if (arguments.size() < 3) {
        feedback(source, "&cUsage: /give <player> <itemId> [count] [damage]");
        return;
    }
    const auto target = server_.players().byName(arguments[1]);
    if (!target) {
        feedback(source, "&cPlayer not found: &f" + arguments[1]);
        return;
    }
    const auto itemId = parseInt(arguments[2]);
    if (!itemId || !mc::isValidItemId(static_cast<mc::i16>(*itemId))) {
        feedback(source, "&cInvalid item id");
        return;
    }
    int count = arguments.size() > 3 ? (parseInt(arguments[3]).value_or(1)) : 1;
    int damage = arguments.size() > 4 ? (parseInt(arguments[4]).value_or(0)) : 0;
    count = std::clamp(count, 1, 64);
    damage = std::clamp(damage, 0, 32767);
    const mc::ItemStack stack{static_cast<mc::i16>(*itemId), static_cast<mc::i8>(count), static_cast<mc::i16>(damage)};
    const int remaining = target->addItem(stack);
    if (remaining > 0) {
        feedback(source, "&cInventory is full");
    } else {
        feedback(source, "&aGave &f" + std::to_string(count) + " &aitem &f" + std::to_string(*itemId)
            + " &ato &f" + target->username());
    }
}

void CommandManager::setBlock(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    if (arguments.size() < 5) {
        feedback(source, "&cUsage: /setblock <x> <y> <z> <blockId> [meta]");
        return;
    }
    const auto x = parseInt(arguments[1]);
    const auto y = parseInt(arguments[2]);
    const auto z = parseInt(arguments[3]);
    const auto blockId = parseInt(arguments[4]);
    const int meta = arguments.size() > 5 ? parseInt(arguments[5]).value_or(0) : 0;
    if (!x || !y || !z || !blockId || *blockId < 0 || *blockId > 255) {
        feedback(source, "&cInvalid block coordinates or id");
        return;
    }
    const u16 state = static_cast<u16>((*blockId << 4) | (std::clamp(meta, 0, 15) & 0x0F));
    server_.setBlockState(*x, *y, *z, state);
    feedback(source, "&aBlock set at &f" + std::to_string(*x) + " " + std::to_string(*y) + " "
        + std::to_string(*z));
}

void CommandManager::setFood(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    if (arguments.size() < 3) {
        feedback(source, "&cUsage: /setfood <player> <0..20>");
        return;
    }
    const auto target = server_.players().byName(arguments[1]);
    if (!target) {
        feedback(source, "&cPlayer not found");
        return;
    }
    const auto value = parseInt(arguments[2]);
    if (!value) {
        feedback(source, "&cInvalid food value");
        return;
    }
    server_.setFoodLevel(*target, *value);
    feedback(source, "&aFood set to &f" + std::to_string(std::clamp(*value, 0, 20)));
}

void CommandManager::script(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        feedback(source, "&cUsage: /script <reload|run> [oracle]");
        return;
    }
    const std::string action = lower(arguments[1]);
    if (action == "reload") {
        if (server_.reloadScripts()) {
            feedback(source, "&aScripts reloaded");
        } else {
            feedback(source, "&cScripts reloaded with errors");
        }
        return;
    }
    if (action == "run") {
        if (arguments.size() < 3) {
            feedback(source, "&cUsage: /script run <oracle>");
            return;
        }
        server_.scripts().fire(arguments[2]);
        feedback(source, "&aOracle dispatched: &f" + arguments[2]);
        return;
    }
    feedback(source, "&cUnknown script action: &f" + action);
}

void CommandManager::modsList(const std::shared_ptr<Connection>& source) {
    const auto mods = server_.mods().list();
    if (mods.empty()) {
        feedback(source, "&eNo mods loaded");
        return;
    }
    feedback(source, "&aLoaded mods (&f" + std::to_string(mods.size()) + "&a):");
    for (const auto& mod : mods) {
        feedback(source, "&f- " + mod);
    }
}

void CommandManager::summon(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        feedback(source, "&cUsage: /summon <type> [x y z]");
        return;
    }
    const auto type = parseInt(arguments[1]);
    if (!type) {
        feedback(source, "&cInvalid mob type");
        return;
    }
    double x = 0.0;
    double y = 4.0;
    double z = 0.0;
    if (arguments.size() >= 5) {
        const auto px = parseDouble(arguments[2]);
        const auto py = parseDouble(arguments[3]);
        const auto pz = parseDouble(arguments[4]);
        if (!px || !py || !pz) {
            feedback(source, "&cInvalid coordinates");
            return;
        }
        x = *px;
        y = *py;
        z = *pz;
    } else if (source) {
        x = source->x() + 1.0;
        y = source->y();
        z = source->z();
    } else {
        feedback(source, "&cConsole must specify coordinates");
        return;
    }
    server_.summonMob(static_cast<u8>(std::clamp(*type, 0, 255)), x, y, z);
    feedback(source, "&aSpawned mob &f" + std::to_string(*type));
}

void CommandManager::heal(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    std::shared_ptr<Connection> target;
    if (arguments.size() > 1) {
        target = server_.players().byName(arguments[1]);
    } else if (source) {
        target = source;
    }
    if (!target) {
        feedback(source, "&cPlayer not found");
        return;
    }
    server_.healPlayer(*target, 20.0F);
    feedback(source, "&aHealed &f" + target->username());
}

void CommandManager::feed(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    std::shared_ptr<Connection> target;
    if (arguments.size() > 1) {
        target = server_.players().byName(arguments[1]);
    } else if (source) {
        target = source;
    }
    if (!target) {
        feedback(source, "&cPlayer not found");
        return;
    }
    server_.feedPlayer(*target, 20);
    feedback(source, "&aFed &f" + target->username());
}

void CommandManager::kill(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    std::shared_ptr<Connection> target;
    if (arguments.size() > 1) {
        target = server_.players().byName(arguments[1]);
    } else if (source) {
        target = source;
    }
    if (!target) {
        feedback(source, "&cPlayer not found");
        return;
    }
    server_.hurtPlayer(*target, 1000.0F);
    feedback(source, "&aKilled &f" + target->username());
}

void CommandManager::clear(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        feedback(source, "&cUsage: /clear <player>");
        return;
    }
    const auto target = server_.players().byName(arguments[1]);
    if (!target) {
        feedback(source, "&cPlayer not found: &f" + arguments[1]);
        return;
    }
    server_.clearPlayerInventory(target);
    feedback(source, "&aCleared inventory of &f" + target->username());
}

bool CommandManager::requireOp(const std::shared_ptr<Connection>& source) {
    if (!source) {
        return true;
    }
    if (server_.isOp(source->username())) {
        return true;
    }
    feedback(source, "&cYou do not have permission to use this command");
    return false;
}

void CommandManager::feedback(const std::shared_ptr<Connection>& source, const std::string& message) {
    if (source) {
        server_.sendSystem(*source, message);
    } else {
        Log::info("{}", chat::plain(message));
    }
}

}
