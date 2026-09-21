#include "vortex/frontends/reference/emitter.hpp"

#include <unordered_map>

#include "vortex/ugb/builder.hpp"

namespace vortex::frontends::reference {

namespace {

// Register allocation strategy (M0 reference port):
//   v0..k-1                    parameters (calling convention: args land here)
//   k..k+63                    let-bound locals (headroom of 64 slots)
//   k+64..                     expression temporaries, recycled per statement
//
// All values flow through canonical opcodes — no speculation is emitted by the
// frontend; the engine's profile-driven rewriter specializes hot sites
// (docs/ugb.md section 4.1, docs/tier-t0.md section 6).
class FunctionEmitter {
public:
    FunctionEmitter(ugb::UGBModule& module, const Function& fn)
        : module_(module), fn_(fn) {}

    Result<void> run() {
        // Calling convention: args land in v[0..argc-1] of the callee frame,
        // so parameters map onto those registers directly; locals continue
        // after them (docs/porting.md, calling convention).
        for (size_t i = 0; i < fn_.params.size(); ++i) {
            locals_[fn_.params[i].name] = static_cast<uint16_t>(i);
        }
        next_local_ = static_cast<uint16_t>(fn_.params.size());
        const uint16_t reg_count =
            static_cast<uint16_t>(kTempBase() + 64);  // temp headroom
        builder_ = std::make_unique<ugb::MethodBuilder>(
            module_, fn_.name, reg_count, static_cast<uint16_t>(fn_.params.size()));

        for (const auto& stmt : fn_.body) {
            auto res = emit_stmt(*stmt);
            if (!res) return res;
        }
        // Implicit `return null` for void functions.
        const uint16_t tmp = temp_reg();
        builder_->const_null(tmp);
        builder_->ret(tmp);
        return builder_->finish();
    }

private:
    static constexpr uint16_t kMaxLocals = 64;
    uint16_t kTempBase() const {
        return static_cast<uint16_t>(fn_.params.size() + kMaxLocals);
    }

    uint16_t local_index(const std::string& name) const {
        auto it = locals_.find(name);
        return it != locals_.end() ? it->second : 0xFFFF;
    }

    uint16_t temp_reg() const {
        return static_cast<uint16_t>(kTempBase() + temp_counter_++);
    }
    void reset_temps() { temp_counter_ = 0; }

    Result<uint16_t> emit_expr(const Expr& e) {
        if (auto* lit = dynamic_cast<const IntLiteral*>(&e)) {
            const uint16_t r = temp_reg();
            if (lit->value >= -2147483648LL && lit->value <= 2147483647LL) {
                builder_->const_i32(r, static_cast<int32_t>(lit->value));
            } else {
                builder_->const_i64(r, lit->value);
            }
            return r;
        }
        if (auto* lit = dynamic_cast<const BoolLiteral*>(&e)) {
            const uint16_t r = temp_reg();
            if (lit->value) {
                builder_->const_true(r);
            } else {
                builder_->const_false(r);
            }
            return r;
        }
        if (dynamic_cast<const NullLiteral*>(&e) != nullptr) {
            const uint16_t r = temp_reg();
            builder_->const_null(r);
            return r;
        }
        if (auto* id = dynamic_cast<const Ident*>(&e)) {
            const uint16_t slot = local_index(id->name);
            if (slot == 0xFFFF) {
                return support::fail(support::ErrorCode::SyntaxError,
                                     "undefined variable '" + id->name + "'",
                                     id->line);
            }
            return slot;
        }
        if (auto* un = dynamic_cast<const Unary*>(&e)) {
            auto operand = emit_expr(*un->operand);
            if (!operand) return operand;
            const uint16_t r = temp_reg();
            if (un->op == '-') {
                const uint16_t zero = temp_reg();
                builder_->const_i32(zero, 0);
                builder_->emit(ugb::Op::SUB_ANY, r, {*operand, zero});
            } else {  // '!'
                const uint16_t t = temp_reg();
                builder_->const_false(t);
                builder_->emit(ugb::Op::EQ_REF, r, {*operand, t});
            }
            return r;
        }
        if (auto* bin = dynamic_cast<const Binary*>(&e)) {
            if (bin->op == "&&" || bin->op == "||") {
                return emit_logical(*bin, bin->op == "&&");
            }
            auto lhs = emit_expr(*bin->lhs);
            if (!lhs) return lhs;
            auto rhs = emit_expr(*bin->rhs);
            if (!rhs) return rhs;
            const uint16_t r = temp_reg();
            const ugb::Op op = binop_opcode(bin->op);
            if (op == ugb::Op::ILLEGAL) {
                return support::fail(support::ErrorCode::SyntaxError,
                                     "unsupported operator '" + bin->op + "'",
                                     bin->line);
            }
            builder_->emit(op, r, {*lhs, *rhs});
            return r;
        }
        if (auto* call = dynamic_cast<const Call*>(&e)) {
            if (call->callee == "print") {
                // print(...) -> Call.Builtin print (docs/porting.md Step 3)
                const uint16_t base = temp_reg();
                uint16_t cur = base;
                for (const auto& a : call->args) {
                    auto v = emit_expr(*a);
                    if (!v) return v;
                    if (*v != cur) {
                        builder_->emit(ugb::Op::MOVE, cur, {*v});
                    }
                    ++cur;
                }
                const uint16_t dst = temp_reg();
                builder_->call_builtin(dst, base,
                                       static_cast<uint16_t>(call->args.size()),
                                       "print");
                return dst;
            }
            const uint16_t base = temp_reg();
            uint16_t cur = base;
            for (const auto& a : call->args) {
                auto v = emit_expr(*a);
                if (!v) return v;
                if (*v != cur) {
                    builder_->emit(ugb::Op::MOVE, cur, {*v});
                }
                ++cur;
            }
            const uint16_t dst = temp_reg();
            builder_->call_direct(dst, base,
                                  static_cast<uint16_t>(call->args.size()),
                                  call->callee);
            return dst;
        }
        return support::fail(support::ErrorCode::SyntaxError,
                             "unsupported expression kind", e.line);
    }

    ugb::Op binop_opcode(const std::string& op) const {
        if (op == "+") return ugb::Op::ADD_ANY;
        if (op == "-") return ugb::Op::SUB_ANY;
        if (op == "*") return ugb::Op::MUL_ANY;
        if (op == "/") return ugb::Op::DIV_S_I64;
        if (op == "%") return ugb::Op::REM_S_I64;
        if (op == "==") return ugb::Op::EQ_I64;
        if (op == "!=") return ugb::Op::NE_I64;
        if (op == "<") return ugb::Op::LT_S_I64;
        if (op == "<=") return ugb::Op::LE_S_I64;
        if (op == ">") return ugb::Op::GT_S_I64;
        if (op == ">=") return ugb::Op::GE_S_I64;
        if (op == "&&" || op == "||") return ugb::Op::ILLEGAL;  // lowered structurally
        return ugb::Op::ILLEGAL;
    }

    // Short-circuit lowering: && and || become branches.
    Result<uint16_t> emit_logical(const Binary& bin, bool is_and) {
        // result = lhs ? rhs : false   (&&)
        // result = lhs ? true  : rhs   (||)
        auto lhs = emit_expr(*bin.lhs);
        if (!lhs) return lhs;
        const uint16_t result = temp_reg();
        const std::string l_false = make_label("logic_false");
        const std::string l_end = make_label("logic_end");
        if (is_and) {
            builder_->jump_false(*lhs, l_false);
        } else {
            builder_->jump_true(*lhs, l_false);
        }
        auto rhs = emit_expr(*bin.rhs);
        if (!rhs) return rhs;
        builder_->emit(ugb::Op::MOVE, result, {*rhs});
        builder_->jump(l_end);
        builder_->bind_label(l_false);
        if (is_and) {
            builder_->const_false(result);
        } else {
            builder_->const_true(result);
        }
        builder_->bind_label(l_end);
        return result;
    }

    Result<void> emit_stmt(const Stmt& s) {
        reset_temps();
        if (auto* let = dynamic_cast<const LetStmt*>(&s)) {
            auto v = emit_expr(*let->value);
            if (!v) return std::unexpected(v.error());
            if (local_index(let->name) == 0xFFFF) {
                if (next_local_ >= kTempBase()) {
                    return support::fail(support::ErrorCode::SyntaxError,
                                         "too many locals in function '" +
                                             fn_.name + "' (max 64)",
                                         let->line);
                }
                locals_[let->name] = next_local_++;
            }
            builder_->emit(ugb::Op::MOVE, locals_[let->name], {*v});
            return support::ok();
        }
        if (auto* assign = dynamic_cast<const AssignStmt*>(&s)) {
            const uint16_t slot = local_index(assign->name);
            if (slot == 0xFFFF) {
                return support::fail(support::ErrorCode::SyntaxError,
                                     "assignment to undeclared '" +
                                         assign->name + "'",
                                     assign->line);
            }
            auto v = emit_expr(*assign->value);
            if (!v) return std::unexpected(v.error());
            builder_->emit(ugb::Op::MOVE, slot, {*v});
            return support::ok();
        }
        if (auto* ifs = dynamic_cast<const IfStmt*>(&s)) {
            auto cond = emit_expr(*ifs->cond);
            if (!cond) return std::unexpected(cond.error());
            const std::string l_else = make_label("if_else");
            const std::string l_end = make_label("if_end");
            builder_->jump_false(*cond, l_else);
            for (const auto& st : ifs->then_body) {
                auto r = emit_stmt(*st);
                if (!r) return r;
            }
            builder_->jump(l_end);
            builder_->bind_label(l_else);
            for (const auto& st : ifs->else_body) {
                auto r = emit_stmt(*st);
                if (!r) return r;
            }
            builder_->bind_label(l_end);
            return support::ok();
        }
        if (auto* wh = dynamic_cast<const WhileStmt*>(&s)) {
            const std::string l_cond = make_label("while_cond");
            const std::string l_end = make_label("while_end");
            builder_->bind_label(l_cond);
            auto cond = emit_expr(*wh->cond);
            if (!cond) return std::unexpected(cond.error());
            builder_->jump_false(*cond, l_end);
            for (const auto& st : wh->body) {
                auto r = emit_stmt(*st);
                if (!r) return r;
            }
            builder_->jump(l_cond);
            builder_->bind_label(l_end);
            return support::ok();
        }
        if (auto* ret = dynamic_cast<const ReturnStmt*>(&s)) {
            if (ret->value) {
                auto v = emit_expr(*ret->value);
                if (!v) return std::unexpected(v.error());
                builder_->ret(*v);
            } else {
                const uint16_t t = temp_reg();
                builder_->const_null(t);
                builder_->ret(t);
            }
            return support::ok();
        }
        if (auto* expr = dynamic_cast<const ExprStmt*>(&s)) {
            auto v = emit_expr(*expr->expr);
            if (!v) return std::unexpected(v.error());
            return support::ok();
        }
        return support::fail(support::ErrorCode::SyntaxError,
                             "unsupported statement kind", s.line);
    }

    std::string make_label(const std::string& hint) {
        return hint + "_" + std::to_string(label_counter_++) + "_" + fn_.name;
    }

    ugb::UGBModule& module_;
    const Function& fn_;
    std::unique_ptr<ugb::MethodBuilder> builder_;
    std::unordered_map<std::string, uint16_t> locals_;
    uint16_t next_local_ = 0;
    mutable uint16_t temp_counter_ = 0;
    uint32_t label_counter_ = 0;
};

}  // namespace

Result<ugb::UGBModule> compile_mini(std::string_view source) {
    auto tokens = lex(source);
    if (!tokens) return std::unexpected(tokens.error());

    Program program;
    try {
        program = parse(*tokens);
    } catch (const ParseError& e) {
        return support::fail(support::ErrorCode::SyntaxError, e.what(), e.line);
    }

    ugb::UGBModule module;
    module.language_name = "mini";
    module.intern_builtin("print");

    for (const auto& fn : program.functions) {
        FunctionEmitter emitter(module, fn);
        auto res = emitter.run();
        if (!res) return std::unexpected(res.error());
    }
    return module;
}

}  // namespace vortex::frontends::reference
