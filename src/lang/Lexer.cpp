#include "lang/Lexer.hpp"

#include <array>
#include <cctype>
#include <utility>

namespace mc::lang {
namespace {

bool isIdentifierStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isIdentifierBody(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

const std::array<std::pair<std::string_view, std::string_view>, 16> kSymbols = {{
    {"::=", "::="},
    {"<=>", "<=>"},
    {"<+>", "<+>"},
    {"<->", "<->"},
    {"<*>", "<*>"},
    {"</>", "</>"},
    {"<%>", "<%>"},
    {"<&>", "<&>"},
    {"<|>", "<|>"},
    {"<^>", "<^>"},
    {"<<>", "<<>"},
    {"<>>", "<>>"},
    {"~>", "~>"},
    {"=>", "=>"},
    {"::", "::"},
    {"", ""},
}};

}

Lexer::Lexer(std::string_view source)
    : source_(source) {
}

bool Lexer::atEnd() const {
    return index_ >= source_.size();
}

char Lexer::peek(std::size_t offset) const {
    const std::size_t position = index_ + offset;
    return position < source_.size() ? source_[position] : '\0';
}

char Lexer::advance() {
    const char c = source_[index_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

void Lexer::skipWhitespace() {
    while (!atEnd() && std::isspace(static_cast<unsigned char>(peek())) != 0) {
        advance();
    }
}

bool Lexer::readIdentifier(std::string& out) {
    if (!isIdentifierStart(peek())) {
        return false;
    }
    out.clear();
    while (!atEnd() && isIdentifierBody(peek())) {
        out.push_back(advance());
    }
    return true;
}

bool Lexer::readNumber(Token& token, std::string& error) {
    advance();
    bool negative = false;
    if (peek() == '-') {
        negative = true;
        advance();
    }
    if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
        error = "line " + std::to_string(token.line) + ", column " + std::to_string(token.column)
            + ": expected digits after #";
        return false;
    }
    std::int64_t value = 0;
    while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        const int digit = advance() - '0';
        if (value > (INT64_MAX - digit) / 10) {
            error = "line " + std::to_string(token.line) + ", column " + std::to_string(token.column)
                + ": number is too large";
            return false;
        }
        value = value * 10 + digit;
    }
    token.kind = TokenKind::Number;
    token.number = negative ? -value : value;
    token.text = std::to_string(token.number);
    return true;
}

bool Lexer::readString(Token& token, std::string& error) {
    advance();
    token.kind = TokenKind::String;
    token.text.clear();
    while (!atEnd()) {
        const char c = advance();
        if (c == '"') {
            return true;
        }
        if (c == '\n') {
            error = "line " + std::to_string(token.line) + ", column " + std::to_string(token.column)
                + ": unterminated string";
            return false;
        }
        if (c == '\\') {
            if (atEnd()) {
                break;
            }
            const char escaped = advance();
            switch (escaped) {
            case 'n':
                token.text.push_back('\n');
                break;
            case 't':
                token.text.push_back('\t');
                break;
            case 'r':
                token.text.push_back('\r');
                break;
            case '\\':
                token.text.push_back('\\');
                break;
            case '"':
                token.text.push_back('"');
                break;
            default:
                error = "line " + std::to_string(token.line) + ", column " + std::to_string(token.column)
                    + ": unknown escape";
                return false;
            }
            continue;
        }
        token.text.push_back(c);
    }
    error = "line " + std::to_string(token.line) + ", column " + std::to_string(token.column)
        + ": unterminated string";
    return false;
}

bool Lexer::readSymbol(Token& token, std::string& error) {
    for (const auto& [symbol, canonical] : kSymbols) {
        if (symbol.empty()) {
            continue;
        }
        if (source_.substr(index_, symbol.size()) == symbol) {
            for (std::size_t i = 0; i < symbol.size(); ++i) {
                advance();
            }
            token.kind = TokenKind::Symbol;
            token.text = std::string(canonical);
            return true;
        }
    }
    const char c = peek();
    if (c == '{' || c == '}' || c == '(' || c == ')' || c == ';' || c == ','
        || c == '[' || c == ']' || c == '+') {
        advance();
        token.kind = TokenKind::Symbol;
        token.text.assign(1, c);
        return true;
    }
    error = "line " + std::to_string(token.line) + ", column " + std::to_string(token.column)
        + ": unexpected character '" + std::string(1, c) + "'";
    return false;
}

bool Lexer::tokenize(std::vector<Token>& tokens, std::string& error) {
    tokens.clear();
    while (true) {
        skipWhitespace();
        if (atEnd()) {
            tokens.push_back(Token{TokenKind::End, "", 0, line_, column_});
            return true;
        }
        Token token;
        token.line = line_;
        token.column = column_;
        const char c = peek();
        if (c == '$') {
            advance();
            std::string name;
            if (!readIdentifier(name)) {
                error = "line " + std::to_string(token.line) + ", column " + std::to_string(token.column)
                    + ": expected register name";
                return false;
            }
            token.kind = TokenKind::Register;
            token.text = std::move(name);
        } else if (c == '@') {
            advance();
            std::string name;
            if (!readIdentifier(name)) {
                error = "line " + std::to_string(token.line) + ", column " + std::to_string(token.column)
                    + ": expected label name";
                return false;
            }
            token.kind = TokenKind::Label;
            token.text = std::move(name);
        } else if (c == '#') {
            if (!readNumber(token, error)) {
                return false;
            }
        } else if (c == '"') {
            if (!readString(token, error)) {
                return false;
            }
        } else if (isIdentifierStart(c)) {
            std::string name;
            readIdentifier(name);
            token.kind = TokenKind::Identifier;
            token.text = std::move(name);
        } else {
            if (!readSymbol(token, error)) {
                return false;
            }
        }
        tokens.push_back(std::move(token));
    }
}

}
