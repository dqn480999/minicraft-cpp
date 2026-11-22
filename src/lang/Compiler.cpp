#include "lang/Compiler.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lang/Lexer.hpp"

namespace mc::lang {
namespace {

struct Operand {
    bool isRegister{false};
    std::uint8_t reg{0};
    Word value{0};
};

struct Address {
    std::uint8_t base{0};
    std::uint8_t index{kNoIndex};
    Word offset{0};
};

struct JumpPatch {
    std::size_t instruction{0};
    std::string label;
};

struct LocalCall {
    std::size_t instruction{0};
    std::string name;
};

std::string tokenText(const Token& token) {
    switch (token.kind) {
    case TokenKind::End:
        return "end of file";
    case TokenKind::Identifier:
        return "identifier " + token.text;
    case TokenKind::Register:
        return "register $" + token.text;
    case TokenKind::Label:
        return "label @" + token.text;
    case TokenKind::Number:
        return "number #" + token.text;
    case TokenKind::String:
        return "string literal";
    case TokenKind::Symbol:
        return "'" + token.text + "'";
    }
    return "token";
}

class Parser {
public:
    Parser(Program& program, std::vector<Token> tokens, std::string origin)
        : program_(program)
        , tokens_(std::move(tokens))
        , origin_(std::move(origin)) {
    }

    bool parse(std::string& error) {
        while (!check(TokenKind::End)) {
            if (!expectIdentifier("ARCANA")) {
                return fail(error, "expected ARCANA, got " + tokenText(current()));
            }
            if (!expect(TokenKind::String)) {
                return fail(error, "expected arcana name, got " + tokenText(current()));
            }
            if (!expectSymbol("{")) {
                return fail(error, "expected {, got " + tokenText(current()));
            }
            while (!checkSymbol("}") && !check(TokenKind::End)) {
                if (!parseOracle(error)) {
                    return false;
                }
            }
            if (!expectSymbol("}")) {
                return fail(error, "expected }, got " + tokenText(current()));
            }
        }
        return true;
    }

private:
    bool parseOracle(std::string& error) {
        if (!expectIdentifier("ORACLE")) {
            return fail(error, "expected ORACLE, got " + tokenText(current()));
        }
        if (!expect(TokenKind::Label)) {
            return fail(error, "expected oracle name, got " + tokenText(current()));
        }
        const std::string name = previous().text;
        if (program_.functions.contains(name)) {
            return fail(error, "duplicate oracle " + name);
        }
        if (!expectSymbol("~>")) {
            return fail(error, "expected ~>, got " + tokenText(current()));
        }
        if (!expectSymbol("{")) {
            return fail(error, "expected {, got " + tokenText(current()));
        }

        program_.functions[name] = program_.code.size();
        labels_.clear();
        jumps_.clear();
        calls_.clear();

        while (!checkSymbol("}") && !check(TokenKind::End)) {
            if (!parseStatement(error)) {
                return false;
            }
        }
        if (!expectSymbol("}")) {
            return fail(error, "expected }, got " + tokenText(current()));
        }

        for (const auto& patch : jumps_) {
            const auto it = labels_.find(patch.label);
            if (it == labels_.end()) {
                return fail(error, "unknown label " + patch.label);
            }
            program_.code[patch.instruction].imm = static_cast<Word>(it->second);
        }
        for (const auto& call : calls_) {
            const auto it = labels_.find(call.name);
            if (it != labels_.end()) {
                program_.code[call.instruction].imm = static_cast<Word>(it->second);
            } else {
                program_.callPatches.push_back(CallPatch{call.instruction, call.name});
            }
        }
        return true;
    }

    bool parseStatement(std::string& error) {
        if (check(TokenKind::Label) && peek(1).kind == TokenKind::Symbol && peek(1).text == "::") {
            const std::string name = advance().text;
            advance();
            if (labels_.contains(name)) {
                return fail(error, "duplicate label " + name);
            }
            labels_[name] = program_.code.size();
            return true;
        }
        if (check(TokenKind::End)) {
            return fail(error, "unexpected end of file");
        }
        if (!check(TokenKind::Identifier)) {
            return fail(error, "expected statement, got " + tokenText(current()));
        }
        const std::string keyword = current().text;
        if (keyword == "BIND") {
            return parseBind(error);
        }
        if (keyword == "REVEAL") {
            return parseReveal(error, false);
        }
        if (keyword == "REVEAL8") {
            return parseReveal(error, true);
        }
        if (keyword == "SCRIBE") {
            return parseScribe(error, false);
        }
        if (keyword == "SCRIBE8") {
            return parseScribe(error, true);
        }
        if (keyword == "VERIFY") {
            return parseVerify(error);
        }
        if (keyword == "GATE") {
            return parseGate(error);
        }
        if (keyword == "WHEN") {
            return parseWhen(error);
        }
        if (keyword == "CALL") {
            return parseCall(error);
        }
        if (keyword == "RETURN") {
            advance();
            if (!expectSymbol(";")) {
                return fail(error, "expected ;, got " + tokenText(current()));
            }
            emit(Op::Ret);
            return true;
        }
        if (keyword == "PUSH" || keyword == "POP") {
            advance();
            const auto reg = parseParenthesizedRegister(error);
            if (!reg) {
                return false;
            }
            if (!expectSymbol(";")) {
                return fail(error, "expected ;, got " + tokenText(current()));
            }
            Instruction instruction;
            instruction.op = keyword == "PUSH" ? Op::Push : Op::Pop;
            instruction.dst = *reg;
            emit(instruction);
            return true;
        }
        const auto unary = unaryOp(keyword);
        if (unary) {
            advance();
            const auto reg = parseParenthesizedRegister(error);
            if (!reg) {
                return false;
            }
            if (!expectSymbol(";")) {
                return fail(error, "expected ;, got " + tokenText(current()));
            }
            Instruction instruction;
            instruction.dst = *reg;
            instruction.op = *unary;
            emit(instruction);
            return true;
        }
        if (keyword == "SWAP") {
            advance();
            if (!expectSymbol("(")) {
                return fail(error, "expected (, got " + tokenText(current()));
            }
            const auto left = parseRegister();
            if (!left) {
                return fail(error, "expected register");
            }
            if (!expectSymbol(",")) {
                return fail(error, "expected , got " + tokenText(current()));
            }
            const auto right = parseRegister();
            if (!right) {
                return fail(error, "expected register");
            }
            if (!expectSymbol(")")) {
                return fail(error, "expected ), got " + tokenText(current()));
            }
            if (!expectSymbol(";")) {
                return fail(error, "expected ;, got " + tokenText(current()));
            }
            Instruction instruction;
            instruction.op = Op::Swap;
            instruction.dst = *left;
            instruction.src = *right;
            emit(instruction);
            return true;
        }
        if (keyword == "SUMMON") {
            advance();
            if (!expectSymbol("(")) {
                return fail(error, "expected (, got " + tokenText(current()));
            }
            const auto id = parseNumber();
            if (!id) {
                return fail(error, "expected host id");
            }
            if (!expectSymbol(")")) {
                return fail(error, "expected ), got " + tokenText(current()));
            }
            if (!expectSymbol(";")) {
                return fail(error, "expected ;, got " + tokenText(current()));
            }
            Instruction instruction;
            instruction.op = Op::Host;
            instruction.imm = *id;
            emit(instruction);
            return true;
        }
        if (keyword == "TRAP") {
            advance();
            if (!expectSymbol("(")) {
                return fail(error, "expected (, got " + tokenText(current()));
            }
            const auto code = parseNumber();
            if (!code) {
                return fail(error, "expected trap code");
            }
            if (!expectSymbol(")")) {
                return fail(error, "expected ), got " + tokenText(current()));
            }
            if (!expectSymbol(";")) {
                return fail(error, "expected ;, got " + tokenText(current()));
            }
            Instruction instruction;
            instruction.op = Op::Trap;
            instruction.imm = *code;
            emit(instruction);
            return true;
        }
        if (keyword == "HALT" || keyword == "NOP") {
            advance();
            if (!expectSymbol(";")) {
                return fail(error, "expected ;, got " + tokenText(current()));
            }
            emit(keyword == "HALT" ? Op::Halt : Op::Nop);
            return true;
        }
        return fail(error, "unknown statement " + keyword);
    }

    bool parseBind(std::string& error) {
        advance();
        const auto dst = parseParenthesizedRegister(error);
        if (!dst) {
            return false;
        }
        if (!expectSymbol("::=")) {
            return fail(error, "expected ::=, got " + tokenText(current()));
        }
        const auto lhs = parseOperand(error);
        if (!lhs) {
            return false;
        }
        if (checkBinaryOperator()) {
            const std::string op = advance().text;
            const auto rhs = parseOperand(error);
            if (!rhs) {
                return false;
            }
            if (!expectSymbol(";")) {
                return fail(error, "expected ;, got " + tokenText(current()));
            }
            emitAssignment(*dst, *lhs);
            Instruction instruction;
            instruction.op = binaryOp(op);
            instruction.dst = *dst;
            encodeRhs(instruction, *rhs);
            emit(instruction);
            return true;
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;, got " + tokenText(current()));
        }
        emitAssignment(*dst, *lhs);
        return true;
    }

    bool parseReveal(std::string& error, bool byte) {
        advance();
        const auto dst = parseParenthesizedRegister(error);
        if (!dst) {
            return false;
        }
        if (!expectSymbol("::=")) {
            return fail(error, "expected ::=, got " + tokenText(current()));
        }
        if (!expectIdentifier(byte ? "MEM8" : "MEM")) {
            return fail(error, "expected memory operand");
        }
        const auto address = parseAddress(error);
        if (!address) {
            return false;
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;, got " + tokenText(current()));
        }
        Instruction instruction;
        instruction.op = byte ? Op::Load8 : Op::Load;
        instruction.dst = *dst;
        instruction.src = address->base;
        instruction.index = address->index;
        instruction.imm = address->offset;
        emit(instruction);
        return true;
    }

    bool parseScribe(std::string& error, bool byte) {
        advance();
        if (!expectIdentifier(byte ? "MEM8" : "MEM")) {
            return fail(error, "expected memory operand");
        }
        const auto address = parseAddress(error);
        if (!address) {
            return false;
        }
        if (!expectSymbol("::=")) {
            return fail(error, "expected ::=, got " + tokenText(current()));
        }
        const auto value = parseOperand(error);
        if (!value) {
            return false;
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;, got " + tokenText(current()));
        }
        Instruction instruction;
        instruction.op = byte ? Op::Store8 : Op::Store;
        instruction.dst = address->base;
        instruction.index = address->index;
        instruction.imm = address->offset;
        if (value->isRegister) {
            instruction.src = value->reg;
        } else {
            instruction.src = kImmediate;
            instruction.imm2 = value->value;
        }
        emit(instruction);
        return true;
    }

    bool parseVerify(std::string& error) {
        advance();
        const auto lhs = parseParenthesizedRegister(error);
        if (!lhs) {
            return false;
        }
        if (!expectSymbol("<=>")) {
            return fail(error, "expected <=>, got " + tokenText(current()));
        }
        const auto rhs = parseOperand(error);
        if (!rhs) {
            return false;
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;, got " + tokenText(current()));
        }
        Instruction instruction;
        instruction.op = Op::Cmp;
        instruction.dst = *lhs;
        encodeRhs(instruction, *rhs);
        emit(instruction);
        return true;
    }

    bool parseGate(std::string& error) {
        advance();
        const auto label = parseLabel();
        if (!label) {
            return fail(error, "expected label, got " + tokenText(current()));
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;, got " + tokenText(current()));
        }
        Instruction instruction;
        instruction.op = Op::Jmp;
        const std::size_t index = emit(instruction);
        jumps_.push_back(JumpPatch{index, *label});
        return true;
    }

    bool parseWhen(std::string& error) {
        advance();
        if (!expectSymbol("::")) {
            return fail(error, "expected ::, got " + tokenText(current()));
        }
        if (!expect(TokenKind::Identifier)) {
            return fail(error, "expected condition, got " + tokenText(current()));
        }
        const std::string condition = previous().text;
        if (!expectSymbol("=>")) {
            return fail(error, "expected =>, got " + tokenText(current()));
        }
        if (!expectIdentifier("GATE")) {
            return fail(error, "expected GATE, got " + tokenText(current()));
        }
        const auto label = parseLabel();
        if (!label) {
            return fail(error, "expected label, got " + tokenText(current()));
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;, got " + tokenText(current()));
        }
        Instruction instruction;
        if (!conditionOp(condition, instruction.op)) {
            return fail(error, "unknown condition " + condition);
        }
        const std::size_t index = emit(instruction);
        jumps_.push_back(JumpPatch{index, *label});
        return true;
    }

    bool parseCall(std::string& error) {
        advance();
        const auto label = parseLabel();
        if (!label) {
            return fail(error, "expected label, got " + tokenText(current()));
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;, got " + tokenText(current()));
        }
        Instruction instruction;
        instruction.op = Op::Call;
        const std::size_t index = emit(instruction);
        calls_.push_back(LocalCall{index, *label});
        return true;
    }

    std::optional<std::uint8_t> parseParenthesizedRegister(std::string& error) {
        if (!expectSymbol("(")) {
            fail(error, "expected (, got " + tokenText(current()));
            return std::nullopt;
        }
        const auto reg = parseRegister();
        if (!reg) {
            fail(error, "expected register, got " + tokenText(current()));
            return std::nullopt;
        }
        if (!expectSymbol(")")) {
            fail(error, "expected ), got " + tokenText(current()));
            return std::nullopt;
        }
        return reg;
    }

    std::optional<Address> parseAddress(std::string& error) {
        if (!expectSymbol("[")) {
            fail(error, "expected [, got " + tokenText(current()));
            return std::nullopt;
        }
        const auto base = parseRegister();
        if (!base) {
            fail(error, "expected base register, got " + tokenText(current()));
            return std::nullopt;
        }
        Address address;
        address.base = *base;
        if (expectSymbol("+")) {
            if (check(TokenKind::Number)) {
                const auto offset = parseNumber();
                address.offset = *offset;
            } else if (check(TokenKind::Register)) {
                const auto index = parseRegister();
                address.index = *index;
            } else {
                fail(error, "expected offset or index register, got " + tokenText(current()));
                return std::nullopt;
            }
        }
        if (!expectSymbol("]")) {
            fail(error, "expected ], got " + tokenText(current()));
            return std::nullopt;
        }
        return address;
    }

    std::optional<Operand> parseOperand(std::string& error) {
        if (check(TokenKind::Register)) {
            const auto reg = parseRegister();
            if (!reg) {
                return std::nullopt;
            }
            return Operand{true, *reg, 0};
        }
        if (check(TokenKind::Number)) {
            const auto value = parseNumber();
            if (!value) {
                return std::nullopt;
            }
            return Operand{false, 0, *value};
        }
        if (check(TokenKind::Identifier) && current().text == "SIGIL") {
            advance();
            if (!expectSymbol("(")) {
                fail(error, "expected (");
                return std::nullopt;
            }
            if (!expect(TokenKind::String)) {
                fail(error, "expected string");
                return std::nullopt;
            }
            const std::string value = previous().text;
            if (!expectSymbol(")")) {
                fail(error, "expected )");
                return std::nullopt;
            }
            const Word id = static_cast<Word>(program_.strings.size());
            program_.strings.push_back(value);
            return Operand{false, 0, id};
        }
        fail(error, "expected operand, got " + tokenText(current()));
        return std::nullopt;
    }

    void emitAssignment(std::uint8_t dst, const Operand& source) {
        Instruction instruction;
        instruction.op = Op::Mov;
        instruction.dst = dst;
        if (source.isRegister) {
            if (source.reg == dst) {
                return;
            }
            instruction.src = source.reg;
        } else {
            instruction.src = kImmediate;
            instruction.imm = source.value;
        }
        emit(instruction);
    }

    void encodeRhs(Instruction& instruction, const Operand& operand) {
        if (operand.isRegister) {
            instruction.src = operand.reg;
        } else {
            instruction.src = kImmediate;
            instruction.imm = operand.value;
        }
    }

    static std::optional<Op> unaryOp(const std::string& keyword) {
        if (keyword == "NEGATE") {
            return Op::Neg;
        }
        if (keyword == "COMPLEMENT") {
            return Op::Not;
        }
        if (keyword == "INCREMENT") {
            return Op::Inc;
        }
        if (keyword == "DECREMENT") {
            return Op::Dec;
        }
        if (keyword == "POPCOUNT") {
            return Op::Popcount;
        }
        if (keyword == "LEADING_ZEROS" || keyword == "CLZ") {
            return Op::Clz;
        }
        if (keyword == "TRAILING_ZEROS" || keyword == "CTZ") {
            return Op::Ctz;
        }
        if (keyword == "BYTESWAP" || keyword == "BSWAP") {
            return Op::Bswap;
        }
        return std::nullopt;
    }

    static Op binaryOp(const std::string& symbol) {
        if (symbol == "<+>") {
            return Op::Add;
        }
        if (symbol == "<->") {
            return Op::Sub;
        }
        if (symbol == "<*>") {
            return Op::Mul;
        }
        if (symbol == "</>") {
            return Op::Div;
        }
        if (symbol == "<%>") {
            return Op::Mod;
        }
        if (symbol == "<&>") {
            return Op::And;
        }
        if (symbol == "<|>") {
            return Op::Or;
        }
        if (symbol == "<^>") {
            return Op::Xor;
        }
        if (symbol == "<<>") {
            return Op::Shl;
        }
        if (symbol == "<rol>") {
            return Op::Rol;
        }
        if (symbol == "<ror>") {
            return Op::Ror;
        }
        return Op::Shr;
    }

    static bool conditionOp(const std::string& name, Op& op) {
        if (name == "EQUAL") {
            op = Op::Je;
            return true;
        }
        if (name == "NOT_EQUAL") {
            op = Op::Jne;
            return true;
        }
        if (name == "LESS") {
            op = Op::Jl;
            return true;
        }
        if (name == "GREATER") {
            op = Op::Jg;
            return true;
        }
        if (name == "LESS_EQUAL" || name == "LESS_OR_EQUAL") {
            op = Op::Jle;
            return true;
        }
        if (name == "GREATER_EQUAL" || name == "GREATER_OR_EQUAL") {
            op = Op::Jge;
            return true;
        }
        if (name == "ZERO") {
            op = Op::Jz;
            return true;
        }
        if (name == "NONZERO") {
            op = Op::Jnz;
            return true;
        }
        if (name == "CARRY") {
            op = Op::Jc;
            return true;
        }
        if (name == "NO_CARRY") {
            op = Op::Jnc;
            return true;
        }
        if (name == "OVERFLOW") {
            op = Op::Jo;
            return true;
        }
        if (name == "NO_OVERFLOW") {
            op = Op::Jno;
            return true;
        }
        return false;
    }

    bool checkBinaryOperator() const {
        if (!check(TokenKind::Symbol)) {
            return false;
        }
        const std::string& text = current().text;
        return text == "<+>" || text == "<->" || text == "<*>" || text == "</>"
            || text == "<%>" || text == "<&>" || text == "<|>" || text == "<^>"
            || text == "<<>" || text == "<>>" || text == "<rol>" || text == "<ror>";
    }

    std::optional<std::uint8_t> parseRegister() {
        if (!check(TokenKind::Register)) {
            return std::nullopt;
        }
        const std::string name = advance().text;
        if (name.size() >= 2 && name[0] == 'r') {
            int value = 0;
            for (std::size_t i = 1; i < name.size(); ++i) {
                if (name[i] < '0' || name[i] > '9') {
                    return std::nullopt;
                }
                value = value * 10 + (name[i] - '0');
            }
            if (value >= 0 && value <= 11) {
                return static_cast<std::uint8_t>(value);
            }
        }
        if (name.size() >= 2 && name[0] == 'a') {
            int value = 0;
            for (std::size_t i = 1; i < name.size(); ++i) {
                if (name[i] < '0' || name[i] > '9') {
                    return std::nullopt;
                }
                value = value * 10 + (name[i] - '0');
            }
            if (value >= 0 && value <= 3) {
                return static_cast<std::uint8_t>(12 + value);
            }
        }
        return std::nullopt;
    }

    std::optional<Word> parseNumber() {
        if (!check(TokenKind::Number)) {
            return std::nullopt;
        }
        return advance().number;
    }

    std::optional<std::string> parseLabel() {
        if (!check(TokenKind::Label)) {
            return std::nullopt;
        }
        return advance().text;
    }

    std::size_t emit(Instruction instruction) {
        if (instruction.line == 0) {
            const Token& token = current();
            instruction.line = static_cast<std::uint32_t>(token.line);
            instruction.column = static_cast<std::uint32_t>(token.column);
        }
        program_.code.push_back(instruction);
        return program_.code.size() - 1;
    }

    std::size_t emit(Op op) {
        Instruction instruction;
        instruction.op = op;
        return emit(instruction);
    }

    bool expectIdentifier(std::string_view text) {
        if (check(TokenKind::Identifier) && current().text == text) {
            advance();
            return true;
        }
        return false;
    }

    bool expectSymbol(std::string_view text) {
        if (check(TokenKind::Symbol) && current().text == text) {
            advance();
            return true;
        }
        return false;
    }

    bool expect(TokenKind kind) {
        if (check(kind)) {
            advance();
            return true;
        }
        return false;
    }

    bool check(TokenKind kind) const {
        return current().kind == kind;
    }

    bool checkSymbol(std::string_view text) const {
        return current().kind == TokenKind::Symbol && current().text == text;
    }

    const Token& current() const {
        return tokens_[index_];
    }

    const Token& previous() const {
        return tokens_[index_ - 1];
    }

    const Token& peek(std::size_t offset) const {
        const std::size_t position = index_ + offset;
        return position < tokens_.size() ? tokens_[position] : tokens_.back();
    }

    const Token& advance() {
        if (index_ < tokens_.size()) {
            ++index_;
        }
        return tokens_[index_ - 1];
    }

    bool fail(std::string& error, const std::string& message) const {
        const Token& token = index_ < tokens_.size() ? tokens_[index_] : tokens_.back();
        error = origin_ + ":" + std::to_string(token.line) + ":" + std::to_string(token.column)
            + ": " + message;
        return false;
    }

    Program& program_;
    std::vector<Token> tokens_;
    std::string origin_;
    std::size_t index_{0};
    std::unordered_map<std::string, std::size_t> labels_;
    std::vector<JumpPatch> jumps_;
    std::vector<LocalCall> calls_;
};

}

bool Compiler::compileInto(Program& program, std::string_view source, std::string_view origin, std::string& error) {
    Lexer lexer(source);
    std::vector<Token> tokens;
    if (!lexer.tokenize(tokens, error)) {
        error = std::string(origin) + ":" + error;
        return false;
    }
    if (program.origin.empty()) {
        program.origin = std::string(origin);
    }
    Parser parser(program, std::move(tokens), std::string(origin));
    return parser.parse(error);
}

}
