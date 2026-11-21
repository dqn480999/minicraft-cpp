#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mc {

class Server;
class Connection;

class CommandManager {
public:
    explicit CommandManager(Server& server);

    void execute(const std::shared_ptr<Connection>& source, std::string_view line);

private:
    void help(const std::shared_ptr<Connection>& source);
    void list(const std::shared_ptr<Connection>& source);
    void say(const std::shared_ptr<Connection>& source, std::string_view message);
    void me(const std::shared_ptr<Connection>& source, std::string_view message);
    void gameMode(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void teleport(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void time(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void seed(const std::shared_ptr<Connection>& source);
    void kick(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void spawn(const std::shared_ptr<Connection>& source);
    void stop(const std::shared_ptr<Connection>& source);
    void opCommand(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments, bool value);
    void info(const std::shared_ptr<Connection>& source);
    void save(const std::shared_ptr<Connection>& source);
    void cleanup(const std::shared_ptr<Connection>& source);
    void give(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void clear(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void setBlock(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void heal(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void feed(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void kill(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void summon(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void setFood(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);
    void script(const std::shared_ptr<Connection>& source, const std::vector<std::string>& arguments);

    bool requireOp(const std::shared_ptr<Connection>& source);
    void feedback(const std::shared_ptr<Connection>& source, const std::string& message);

    Server& server_;
};

}
