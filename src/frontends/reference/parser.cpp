#include "vortex/frontends/reference/parser.hpp"

#include <cctype>
#include <utility>

namespace vortex::frontends::reference {

namespace {

/// Recursive-descent parser with first-error-wins error state (Rule 65: no
/// native exceptions). All parse functions return null/empty on failure and
/// their callers short-circuit after checking `failed()`.
class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : toks_(tokens) {}

    Result<Program> parse_program() {
        Program prog;
        while (!at_end() && !failed()) {
            if (peek().text == "fn") {
                auto f = parse_function();
                if (failed()) return std::unexpected(error_);
                prog.functions.push_back(std::move(*f));
            } else {
                fail("expected 'fn' at top level", peek().line);
            }
        }
        if (failed()) return std::unexpected(error_);
        return prog;
    }

private:
    const std::vector<Token>& toks_;
    size_t pos_ = 0;
    support::Diagnostic error_{};
    bool failed_ = false;

    void fail(std::string msg, uint32_t line) {
        if (failed_) return;
        failed_ = true;
        error_ = support::Diagnostic(support::ErrorCode::SyntaxError, std::move(msg), line);
    }
    bool failed() const { return failed_; }

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
            fail("expected '" + text + "', got '" + peek().text + "'",
                 peek().line);
            static const Token kInvalid{};
            return kInvalid;
        }
        return next();
    }

    std::unique_ptr<Function> parse_function() {
        auto f = std::make_unique<Function>();
        f->line = peek().line;
        expect("fn");
        if (failed()) return nullptr;
        if (peek().kind != TokKind::Ident && peek().text != "print") {
            fail("expected function name", peek().line);
            return nullptr;
        }
        f->name = next().text;
        expect("(");
        if (failed()) return nullptr;
        if (peek().text != ")") {
            while (true) {
                if (failed()) return nullptr;
                if (peek().kind != TokKind::Ident) {
                    fail("expected parameter name", peek().line);
                    return nullptr;
                }
                f->params.push_back(Param{next().text});
                if (!match(",")) break;
            }
        }
        expect(")");
        if (failed()) return nullptr;
        f->body = parse_block();
        if (failed()) return nullptr;
        return f;
    }

    std::vector<StmtPtr> parse_block() {
        std::vector<StmtPtr> body;
        expect("{");
        while (!failed() && !at_end() && peek().text != "}") {
            auto stmt = parse_statement();
            if (failed()) return body;
            body.push_back(std::move(stmt));
        }
        expect("}");
        return body;
    }

    StmtPtr parse_statement() {
        const uint32_t line = peek().line;
        if (peek().text == "let") {
            next();
            if (peek().kind != TokKind::Ident) {
                fail("expected binding name after 'let'", line);
                return nullptr;
            }
            auto stmt = std::make_unique<LetStmt>();
            stmt->name = next().text;
            expect("=");
            if (failed()) return nullptr;
            stmt->value = parse_expression();
            if (failed()) return nullptr;
            expect(";");
            if (failed()) return nullptr;
            stmt->line = line;
            return stmt;
        }
        if (peek().text == "if") {
            next();
            auto stmt = std::make_unique<IfStmt>();
            expect("(");
            if (failed()) return nullptr;
            stmt->cond = parse_expression();
            if (failed()) return nullptr;
            expect(")");
            if (failed()) return nullptr;
            stmt->then_body = parse_block();
            if (failed()) return nullptr;
            if (match("else")) {
                if (peek().text == "if") {
                    // else-if chain: wrap as nested if
                    auto nested = parse_statement();
                    if (failed()) return nullptr;
                    stmt->else_body.push_back(std::move(nested));
                } else {
                    stmt->else_body = parse_block();
                    if (failed()) return nullptr;
                }
            }
            stmt->line = line;
            return stmt;
        }
        if (peek().text == "while") {
            next();
            auto stmt = std::make_unique<WhileStmt>();
            expect("(");
            if (failed()) return nullptr;
            stmt->cond = parse_expression();
            if (failed()) return nullptr;
            expect(")");
            if (failed()) return nullptr;
            stmt->body = parse_block();
            if (failed()) return nullptr;
            stmt->line = line;
            return stmt;
        }
        if (peek().text == "return") {
            next();
            auto stmt = std::make_unique<ReturnStmt>();
            if (!at_end() && peek().text != ";") {
                stmt->value = parse_expression();
                if (failed()) return nullptr;
            }
            expect(";");
            if (failed()) return nullptr;
            stmt->line = line;
            return stmt;
        }
        // Expression statement (assignment or call).
        if (peek().kind == TokKind::Ident && peek(1).text == "=") {
            auto assign = std::make_unique<AssignStmt>();
            assign->name = next().text;
            next();  // '='
            assign->value = parse_expression();
            if (failed()) return nullptr;
            expect(";");
            if (failed()) return nullptr;
            assign->line = line;
            return assign;
        }
        auto stmt = std::make_unique<ExprStmt>();
        stmt->expr = parse_expression();
        if (failed()) return nullptr;
        expect(";");
        if (failed()) return nullptr;
        stmt->line = line;
        return stmt;
    }

    // ---- expressions (precedence climbing) -----------------------------------

    ExprPtr parse_expression() { return parse_or(); }

    ExprPtr parse_or() {
        auto lhs = parse_and();
        while (!failed() && peek().text == "||") {
            const uint32_t line = next().line;
            auto e = std::make_unique<Binary>();
            e->op = "||";
            e->lhs = std::move(lhs);
            e->rhs = parse_and();
            if (failed()) return nullptr;
            e->line = line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_and() {
        auto lhs = parse_equality();
        while (!failed() && peek().text == "&&") {
            const uint32_t line = next().line;
            auto e = std::make_unique<Binary>();
            e->op = "&&";
            e->lhs = std::move(lhs);
            e->rhs = parse_equality();
            if (failed()) return nullptr;
            e->line = line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_equality() {
        auto lhs = parse_comparison();
        while (!failed() && (peek().text == "==" || peek().text == "!=")) {
            const std::string op = next().text;
            const uint32_t line = peek().line;
            auto e = std::make_unique<Binary>();
            e->op = op;
            e->lhs = std::move(lhs);
            e->rhs = parse_comparison();
            if (failed()) return nullptr;
            e->line = line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_comparison() {
        auto lhs = parse_additive();
        while (!failed() && (peek().text == "<" || peek().text == "<=" ||
                             peek().text == ">" || peek().text == ">=")) {
            const std::string op = next().text;
            const uint32_t line = peek().line;
            auto e = std::make_unique<Binary>();
            e->op = op;
            e->lhs = std::move(lhs);
            e->rhs = parse_additive();
            if (failed()) return nullptr;
            e->line = line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_additive() {
        auto lhs = parse_multiplicative();
        while (!failed() && (peek().text == "+" || peek().text == "-")) {
            const std::string op = next().text;
            auto e = std::make_unique<Binary>();
            e->op = op;
            e->lhs = std::move(lhs);
            e->rhs = parse_multiplicative();
            if (failed()) return nullptr;
            e->line = peek().line;
            lhs = std::move(e);
        }
        return lhs;
    }

    ExprPtr parse_multiplicative() {
        auto lhs = parse_unary();
        while (!failed() &&
               (peek().text == "*" || peek().text == "/" || peek().text == "%")) {
            const std::string op = next().text;
            auto e = std::make_unique<Binary>();
            e->op = op;
            e->lhs = std::move(lhs);
            e->rhs = parse_unary();
            if (failed()) return nullptr;
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
            if (failed()) return nullptr;
            e->line = line;
            return e;
        }
        if (peek().text == "!") {
            const uint32_t line = next().line;
            auto e = std::make_unique<Unary>();
            e->op = '!';
            e->operand = parse_unary();
            if (failed()) return nullptr;
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
            // Guard the lookahead: never index past the token stream.
            if (peek(1).text == "(") {
                auto e = std::make_unique<Call>();
                e->callee = next().text;
                next();  // '('
                if (peek().text != ")") {
                    while (!failed()) {
                        auto arg = parse_expression();
                        if (failed()) return nullptr;
                        e->args.push_back(std::move(arg));
                        if (!match(",")) break;
                    }
                }
                expect(")");
                if (failed()) return nullptr;
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
            if (failed()) return nullptr;
            expect(")");
            if (failed()) return nullptr;
            return e;
        }
        fail("unexpected token '" + t.text + "'", t.line);
        return nullptr;
    }
};

}  // namespace

Result<Program> parse(const std::vector<Token>& tokens) {
    Parser p(tokens);
    return p.parse_program();
}

}  // namespace vortex::frontends::reference
