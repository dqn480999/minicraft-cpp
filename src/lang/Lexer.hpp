#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc::lang {

enum class TokenKind {
    End,
    Identifier,
    Register,
    Label,
    Number,
    String,
    Symbol,
};

struct Token {
    TokenKind kind{TokenKind::End};
    std::string text;
    std::int64_t number{0};
    int line{1};
    int column{1};
};

class Lexer {
public:
    explicit Lexer(std::string_view source);

    bool tokenize(std::vector<Token>& tokens, std::string& error);

private:
    bool atEnd() const;
    char peek(std::size_t offset = 0) const;
    char advance();
    void skipWhitespace();
    bool readIdentifier(std::string& out);
    bool readNumber(Token& token, std::string& error);
    bool readString(Token& token, std::string& error);
    bool readSymbol(Token& token, std::string& error);

    std::string_view source_;
    std::size_t index_{0};
    int line_{1};
    int column_{1};
};

}
