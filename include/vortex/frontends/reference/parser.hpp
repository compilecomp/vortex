// Mini reference frontend — AST + recursive-descent parser.
//
// Rule 68: AST nodes carry an explicit kind enum; dispatch uses kind tags,
// never dynamic_cast/typeid. Rule 65: the parser never throws — syntax
// errors are returned as Result (first error wins).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "vortex/frontends/reference/lexer.hpp"
#include "vortex/support/result.hpp"

namespace vortex::frontends::reference {

// ---- AST -------------------------------------------------------------------

enum class ExprKind : uint8_t {
    IntLiteral,
    BoolLiteral,
    NullLiteral,
    Ident,
    Unary,
    Binary,
    Call,
};

enum class StmtKind : uint8_t {
    Let,
    Assign,
    If,
    While,
    Return,
    ExprStmt,
};

struct Expr {
    explicit Expr(ExprKind k) noexcept : kind(k) {}
    virtual ~Expr() = default;
    ExprKind kind;
    uint32_t line = 0;
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLiteral : Expr {
    IntLiteral() : Expr(ExprKind::IntLiteral) {}
    int64_t value = 0;
};
struct BoolLiteral : Expr {
    BoolLiteral() : Expr(ExprKind::BoolLiteral) {}
    bool value = false;
};
struct NullLiteral : Expr {
    NullLiteral() : Expr(ExprKind::NullLiteral) {}
};
struct Ident : Expr {
    Ident() : Expr(ExprKind::Ident) {}
    std::string name;
};
struct Unary : Expr {
    Unary() : Expr(ExprKind::Unary) {}
    char op = 0;  // '-', '!'
    ExprPtr operand;
};
struct Binary : Expr {
    Binary() : Expr(ExprKind::Binary) {}
    std::string op;  // "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "&&", "||"
    ExprPtr lhs;
    ExprPtr rhs;
};
struct Call : Expr {
    Call() : Expr(ExprKind::Call) {}
    std::string callee;
    std::vector<ExprPtr> args;
};

struct Stmt {
    explicit Stmt(StmtKind k) noexcept : kind(k) {}
    virtual ~Stmt() = default;
    StmtKind kind;
    uint32_t line = 0;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct LetStmt : Stmt {
    LetStmt() : Stmt(StmtKind::Let) {}
    std::string name;
    ExprPtr value;
};
struct AssignStmt : Stmt {
    AssignStmt() : Stmt(StmtKind::Assign) {}
    std::string name;
    ExprPtr value;
};
struct IfStmt : Stmt {
    IfStmt() : Stmt(StmtKind::If) {}
    ExprPtr cond;
    std::vector<StmtPtr> then_body;
    std::vector<StmtPtr> else_body;
};
struct WhileStmt : Stmt {
    WhileStmt() : Stmt(StmtKind::While) {}
    ExprPtr cond;
    std::vector<StmtPtr> body;
};
struct ReturnStmt : Stmt {
    ReturnStmt() : Stmt(StmtKind::Return) {}
    ExprPtr value;  // may be null -> Return.Unit
};
struct ExprStmt : Stmt {
    ExprStmt() : Stmt(StmtKind::ExprStmt) {}
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

/// Parses Mini source into a Program. Never throws (Rule 65): syntax errors
/// come back as a Diagnostic with line information, first error wins.
Result<Program> parse(const std::vector<Token>& tokens);

}  // namespace vortex::frontends::reference
