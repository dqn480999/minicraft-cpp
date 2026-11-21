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

struct JumpPatch {
    std::size_t instruction{0};
    std::string label;
};

struct LocalCall {
    std::size_t instruction{0};
    std::string name;
};

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
                return fail(error, "expected ARCANA");
            }
            if (!expect(TokenKind::String)) {
                return fail(error, "expected arcana name");
            }
            if (!expectSymbol("{")) {
                return fail(error, "expected {");
            }
            while (!checkSymbol("}") && !check(TokenKind::End)) {
                if (!parseOracle(error)) {
                    return false;
                }
            }
            if (!expectSymbol("}")) {
                return fail(error, "expected }");
            }
        }
        return true;
    }

private:
    bool parseOracle(std::string& error) {
        if (!expectIdentifier("ORACLE")) {
            return fail(error, "expected ORACLE");
        }
        if (!expect(TokenKind::Label)) {
            return fail(error, "expected oracle name");
        }
        const std::string name = previous().text;
        if (program_.functions.contains(name)) {
            return fail(error, "duplicate oracle " + name);
        }
        if (!expectSymbol("~>")) {
            return fail(error, "expected ~>");
        }
        if (!expectSymbol("{")) {
            return fail(error, "expected {");
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
            return fail(error, "expected }");
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
            return fail(error, "expected statement");
        }
        const std::string keyword = current().text;
        if (keyword == "BIND") {
            return parseBind(error);
        }
        if (keyword == "REVEAL") {
            return parseReveal(error);
        }
        if (keyword == "SCRIBE") {
            return parseScribe(error);
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
                return fail(error, "expected ;");
            }
            emit(Op::Ret);
            return true;
        }
        if (keyword == "PUSH" || keyword == "POP") {
            advance();
            if (!expectSymbol("(")) {
                return fail(error, "expected (");
            }
            const auto reg = parseRegister();
            if (!reg) {
                return fail(error, "expected register");
            }
            if (!expectSymbol(")")) {
                return fail(error, "expected )");
            }
            if (!expectSymbol(";")) {
                return fail(error, "expected ;");
            }
            Instruction instruction;
            instruction.op = keyword == "PUSH" ? Op::Push : Op::Pop;
            instruction.dst = *reg;
            emit(instruction);
            return true;
        }
        if (keyword == "SUMMON") {
            advance();
            if (!expectSymbol("(")) {
                return fail(error, "expected (");
            }
            const auto id = parseNumber();
            if (!id) {
                return fail(error, "expected host id");
            }
            if (!expectSymbol(")")) {
                return fail(error, "expected )");
            }
            if (!expectSymbol(";")) {
                return fail(error, "expected ;");
            }
            Instruction instruction;
            instruction.op = Op::Host;
            instruction.imm = *id;
            emit(instruction);
            return true;
        }
        if (keyword == "HALT" || keyword == "NOP") {
            advance();
            if (!expectSymbol(";")) {
                return fail(error, "expected ;");
            }
            emit(keyword == "HALT" ? Op::Halt : Op::Nop);
            return true;
        }
        return fail(error, "unknown statement " + keyword);
    }

    bool parseBind(std::string& error) {
        advance();
        if (!expectSymbol("(")) {
            return fail(error, "expected (");
        }
        const auto dst = parseRegister();
        if (!dst) {
            return fail(error, "expected destination register");
        }
        if (!expectSymbol(")")) {
            return fail(error, "expected )");
        }
        if (!expectSymbol("::=")) {
            return fail(error, "expected ::=");
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
                return fail(error, "expected ;");
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
            return fail(error, "expected ;");
        }
        emitAssignment(*dst, *lhs);
        return true;
    }

    bool parseReveal(std::string& error) {
        advance();
        if (!expectSymbol("(")) {
            return fail(error, "expected (");
        }
        const auto dst = parseRegister();
        if (!dst) {
            return fail(error, "expected register");
        }
        if (!expectSymbol(")")) {
            return fail(error, "expected )");
        }
        if (!expectSymbol("::=")) {
            return fail(error, "expected ::=");
        }
        if (!expectIdentifier("MEM")) {
            return fail(error, "expected MEM");
        }
        if (!expectSymbol("[")) {
            return fail(error, "expected [");
        }
        const auto base = parseRegister();
        if (!base) {
            return fail(error, "expected base register");
        }
        if (!expectSymbol("+")) {
            return fail(error, "expected +");
        }
        const auto offset = parseNumber();
        if (!offset) {
            return fail(error, "expected offset");
        }
        if (!expectSymbol("]")) {
            return fail(error, "expected ]");
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;");
        }
        Instruction instruction;
        instruction.op = Op::Load;
        instruction.dst = *dst;
        instruction.src = *base;
        instruction.imm = *offset;
        emit(instruction);
        return true;
    }

    bool parseScribe(std::string& error) {
        advance();
        if (!expectIdentifier("MEM")) {
            return fail(error, "expected MEM");
        }
        if (!expectSymbol("[")) {
            return fail(error, "expected [");
        }
        const auto base = parseRegister();
        if (!base) {
            return fail(error, "expected base register");
        }
        if (!expectSymbol("+")) {
            return fail(error, "expected +");
        }
        const auto offset = parseNumber();
        if (!offset) {
            return fail(error, "expected offset");
        }
        if (!expectSymbol("]")) {
            return fail(error, "expected ]");
        }
        if (!expectSymbol("::=")) {
            return fail(error, "expected ::=");
        }
        const auto value = parseOperand(error);
        if (!value) {
            return false;
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;");
        }
        Instruction instruction;
        instruction.op = Op::Store;
        instruction.dst = *base;
        instruction.imm = *offset;
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
        if (!expectSymbol("(")) {
            return fail(error, "expected (");
        }
        const auto lhs = parseRegister();
        if (!lhs) {
            return fail(error, "expected register");
        }
        if (!expectSymbol(")")) {
            return fail(error, "expected )");
        }
        if (!expectSymbol("<=>")) {
            return fail(error, "expected <=>");
        }
        const auto rhs = parseOperand(error);
        if (!rhs) {
            return false;
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;");
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
            return fail(error, "expected label");
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;");
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
            return fail(error, "expected ::");
        }
        if (!expect(TokenKind::Identifier)) {
            return fail(error, "expected condition");
        }
        const std::string condition = previous().text;
        if (!expectSymbol("=>")) {
            return fail(error, "expected =>");
        }
        if (!expectIdentifier("GATE")) {
            return fail(error, "expected GATE");
        }
        const auto label = parseLabel();
        if (!label) {
            return fail(error, "expected label");
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;");
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
            return fail(error, "expected label");
        }
        if (!expectSymbol(";")) {
            return fail(error, "expected ;");
        }
        Instruction instruction;
        instruction.op = Op::Call;
        const std::size_t index = emit(instruction);
        calls_.push_back(LocalCall{index, *label});
        return true;
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
        fail(error, "expected operand");
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
        return Op::Shr;
    }

    static bool conditionOp(const std::string& name, Op& op) {
        if (name == "EQUAL" || name == "ZERO") {
            op = Op::Je;
            return true;
        }
        if (name == "NOT_EQUAL" || name == "NONZERO") {
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
        return false;
    }

    bool checkBinaryOperator() const {
        if (!check(TokenKind::Symbol)) {
            return false;
        }
        const std::string& text = current().text;
        return text == "<+>" || text == "<->" || text == "<*>" || text == "</>"
            || text == "<%>" || text == "<&>" || text == "<|>" || text == "<^>"
            || text == "<<>" || text == "<>>";
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
        error = origin_ + ":" + std::to_string(current().line) + ":" + std::to_string(current().column)
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
