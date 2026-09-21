// Mini reference frontend — AST + recursive-descent parser.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "vortex/frontends/reference/lexer.hpp"

namespace vortex::frontends::reference {

// ---- AST -------------------------------------------------------------------

struct Expr {
    virtual ~Expr() = default;
    uint32_t line = 0;
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLiteral : Expr {
    int64_t value = 0;
};
struct BoolLiteral : Expr {
    bool value = false;
};
struct NullLiteral : Expr {};
struct Ident : Expr {
    std::string name;
};
struct Unary : Expr {
    char op = 0;  // '-', '!'
    ExprPtr operand;
};
struct Binary : Expr {
    std::string op;  // "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&&", "||"
    ExprPtr lhs;
    ExprPtr rhs;
};
struct Call : Expr {
    std::string callee;
    std::vector<ExprPtr> args;
};

struct Stmt {
    virtual ~Stmt() = default;
    uint32_t line = 0;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct LetStmt : Stmt {
    std::string name;
    ExprPtr value;
};
struct AssignStmt : Stmt {
    std::string name;
    ExprPtr value;
};
struct IfStmt : Stmt {
    ExprPtr cond;
    std::vector<StmtPtr> then_body;
    std::vector<StmtPtr> else_body;
};
struct WhileStmt : Stmt {
    ExprPtr cond;
    std::vector<StmtPtr> body;
};
struct ReturnStmt : Stmt {
    ExprPtr value;  // may be null -> Return.Unit
};
struct ExprStmt : Stmt {
    ExprPtr expr;
};

struct Param {
    std::string name;
};

struct Function {
    std::string name;
    std::vector<Param> params;
    std::vector<StmtPtr> body;
    uint32_t line = 0;
};

struct Program {
    std::vector<Function> functions;
};

// ---- parser ------------------------------------------------------------------

class ParseError : public std::runtime_error {
public:
    ParseError(std::string msg, uint32_t line)
        : std::runtime_error(std::move(msg)), line(line) {}
    uint32_t line;
};

/// Parses Mini source into a Program. Throws ParseError on syntax errors.
Program parse(const std::vector<Token>& tokens);

}  // namespace vortex::frontends::reference
