#include "vortex/frontends/reference/parser.hpp"

#include <cctype>

namespace vortex::frontends::reference {

namespace {

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : toks_(tokens) {}

    Program parse_program() {
        Program prog;
        while (!at_end()) {
            if (peek().text == "fn") {
                prog.functions.push_back(parse_function());
            } else {
                throw ParseError("expected 'fn' at top level", peek().line);
            }
        }
        return prog;
    }

private:
    const std::vector<Token>& toks_;
    size_t pos_ = 0;

    const Token& peek(size_t off = 0) const {
        const size_t i = pos_ + off;
        return i < toks_.size() ? toks_[i] : toks_.back();
    }
    const Token& next() { return toks_[pos_ < toks_.size() - 1 ? pos_++ : pos_]; }
    bool at_end() const { return peek().kind == TokKind::End; }
    bool match(const std::string& text) {
        if (!at_end() && peek().text == text) {
            ++pos_;
            return true;
        }
        return false;
    }
    const Token& expect(const std::string& text) {
        if (at_end() || peek().text != text) {
            throw ParseError("expected '" + text + "', got '" + peek().text + "'",
                             peek().line);
        }
        return next();
    }

    Function parse_function() {
        Function f;
        f.line = peek().line;
        expect("fn");
        if (peek().kind != TokKind::Ident && peek().text != "print") {
            throw ParseError("expected function name", peek().line);
        }
        f.name = next().text;
        expect("(");
        if (peek().text != ")") {
            while (true) {
                if (peek().kind != TokKind::Ident) {
                    throw ParseError("expected parameter name", peek().line);
                }
                f.params.push_back(Param{next().text});
                if (!match(",")) break;
            }
        }
        expect(")");
        f.body = parse_block();
        return f;
    }

    std::vector<StmtPtr> parse_block() {
        expect("{");
        std::vector<StmtPtr> body;
        while (!at_end() && peek().text != "}") {
            body.push_back(parse_statement());
        }
        expect("}");
        return body;
    }

    StmtPtr parse_statement() {
        const uint32_t line = peek().line;
        if (peek().text == "let") {
            next();
            if (peek().kind != TokKind::Ident) {
                throw ParseError("expected binding name after 'let'", line);
            }
            auto stmt = std::make_unique<LetStmt>();
            stmt->name = next().text;
            expect("=");
            stmt->value = parse_expression();
            expect(";");
            stmt->line = line;
            return stmt;
        }
        if (peek().text == "if") {
            next();
            auto stmt = std::make_unique<IfStmt>();
            expect("(");
            stmt->cond = parse_expression();
            expect(")");
            stmt->then_body = parse_block();
            if (match("else")) {
                if (peek().text == "if") {
                    // else-if chain: wrap as nested if
                    auto nested = parse_statement();
                    stmt->else_body.push_back(std::move(nested));
                } else {
                    stmt->else_body = parse_block();
                }
            }
            stmt->line = line;
            return stmt;
        }
        if (peek().text == "while") {
            next();
            auto stmt = std::make_unique<WhileStmt>();
            expect("(");
            stmt->cond = parse_expression();
            expect(")");
            stmt->body = parse_block();
            stmt->line = line;
            return stmt;
        }
        if (peek().text == "return") {
            next();
            auto stmt = std::make_unique<ReturnStmt>();
            if (!at_end() && peek().text != ";") {
                stmt->value = parse_expression();
            }
            expect(";");
            stmt->line = line;
            return stmt;
        }
        // Expression statement (assignment or call).
        auto stmt = std::make_unique<ExprStmt>();
        if (peek().kind == TokKind::Ident && peek(1).text == "=") {
            auto assign = std::make_unique<AssignStmt>();
            assign->name = next().text;
            next();  // '='
            assign->value = parse_expression();
            expect(";");
            assign->line = line;
            return assign;
        }
        stmt->expr = parse_expression();
        expect(";");
        stmt->line = line;
        return stmt;
    }

    // ---- expressions (precedence climbing) -----------------------------------

    ExprPtr parse_expression() { return parse_or(); }

    ExprPtr parse_or() {
        auto lhs = parse_and();
        while (peek().text == "||") {
            const uint32_t line = next().line;
            auto e = std::make_unique<Binary>();
            e->op = "||";
            e->lhs = std::move(lhs);
            e->rhs = parse_and();
            e->line = line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_and() {
        auto lhs = parse_equality();
        while (peek().text == "&&") {
            const uint32_t line = next().line;
            auto e = std::make_unique<Binary>();
            e->op = "&&";
            e->lhs = std::move(lhs);
            e->rhs = parse_equality();
            e->line = line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_equality() {
        auto lhs = parse_comparison();
        while (peek().text == "==" || peek().text == "!=") {
            const std::string op = next().text;
            const uint32_t line = peek().line;
            auto e = std::make_unique<Binary>();
            e->op = op;
            e->lhs = std::move(lhs);
            e->rhs = parse_comparison();
            e->line = line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_comparison() {
        auto lhs = parse_additive();
        while (peek().text == "<" || peek().text == "<=" ||
               peek().text == ">" || peek().text == ">=") {
            const std::string op = next().text;
            const uint32_t line = peek().line;
            auto e = std::make_unique<Binary>();
            e->op = op;
            e->lhs = std::move(lhs);
            e->rhs = parse_additive();
            e->line = line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_additive() {
        auto lhs = parse_multiplicative();
        while (peek().text == "+" || peek().text == "-") {
            const std::string op = next().text;
            auto e = std::make_unique<Binary>();
            e->op = op;
            e->lhs = std::move(lhs);
            e->rhs = parse_multiplicative();
            e->line = peek().line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_multiplicative() {
        auto lhs = parse_unary();
        while (peek().text == "*" || peek().text == "/" || peek().text == "%") {
            const std::string op = next().text;
            auto e = std::make_unique<Binary>();
            e->op = op;
            e->lhs = std::move(lhs);
            e->rhs = parse_unary();
            e->line = peek().line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_unary() {
        if (peek().text == "-") {
            const uint32_t line = next().line;
            auto e = std::make_unique<Unary>();
            e->op = '-';
            e->operand = parse_unary();
            e->line = line;
            return e;
        }
        if (peek().text == "!") {
            const uint32_t line = next().line;
            auto e = std::make_unique<Unary>();
            e->op = '!';
            e->operand = parse_unary();
            e->line = line;
            return e;
        }
        return parse_primary();
    }

    ExprPtr parse_primary() {
        const Token& t = peek();
        if (t.kind == TokKind::Int) {
            auto e = std::make_unique<IntLiteral>();
            e->value = next().int_value;
            e->line = t.line;
            return e;
        }
        if (t.text == "true" || t.text == "false") {
            auto e = std::make_unique<BoolLiteral>();
            e->value = t.text == "true";
            next();
            e->line = t.line;
            return e;
        }
        if (t.text == "null") {
            next();
            auto e = std::make_unique<NullLiteral>();
            e->line = t.line;
            return e;
        }
        if (t.kind == TokKind::Ident ||
            (t.kind == TokKind::Keyword && t.text == "print")) {
            if (toks_[pos_ + 1].text == "(") {
                auto e = std::make_unique<Call>();
                e->callee = next().text;
                next();  // '('
                if (peek().text != ")") {
                    while (true) {
                        e->args.push_back(parse_expression());
                        if (!match(",")) break;
                    }
                }
                expect(")");
                e->line = t.line;
                return e;
            }
            auto e = std::make_unique<Ident>();
            e->name = next().text;
            e->line = t.line;
            return e;
        }
        if (t.text == "(") {
            next();
            auto e = parse_expression();
            expect(")");
            return e;
        }
        throw ParseError("unexpected token '" + t.text + "'", t.line);
    }
};

}  // namespace

Program parse(const std::vector<Token>& tokens) {
    Parser p(tokens);
    return p.parse_program();
}

}  // namespace vortex::frontends::reference
