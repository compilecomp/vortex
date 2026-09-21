// Mini reference frontend — lexer.
//
// Mini is NOT a product language. It is the smallest frontend that demonstrates
// a language port into UGB (docs/porting.md section 4): functions, let bindings,
// if/else, while, integer/boolean/null literals, arithmetic, comparisons,
// logic, and calls. Kept under frontends/reference/ as a test fixture.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/support/result.hpp"

namespace vortex::frontends::reference {

using support::Result;

enum class TokKind : uint8_t {
    Ident,
    Int,
    Keyword,
    Punct,
    End,
};

struct Token {
    TokKind kind = TokKind::End;
    std::string text;
    int64_t int_value = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

struct LexDiagnostic {
    std::string message;
    uint32_t line = 0;
};

/// Tokenizes Mini source. Punctuators: ( ) { } , ; + - * / % == != < <= > >=
/// && || ! =. Keywords: fn let if else while return true false null print.
Result<std::vector<Token>> lex(std::string_view source);

}  // namespace vortex::frontends::reference
