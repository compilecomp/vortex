#include "vortex/frontends/reference/lexer.hpp"

#include <cctype>

namespace vortex::frontends::reference {

namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool is_keyword(const std::string& t) {
    static const char* kKeywords[] = {"fn",  "let",    "if",   "else", "while",
                                      "return", "true", "false", "null", "print"};
    for (const char* k : kKeywords) {
        if (t == k) return true;
    }
    return false;
}

// Two-char punctuators first.
bool match_two_char(std::string_view s, size_t i, std::string& out) {
    if (i + 1 >= s.size()) return false;
    const char a = s[i], b = s[i + 1];
    const std::string two{s[i], s[i + 1]};
    if (two == "==" || two == "!=" || two == "<=" || two == ">=" ||
        two == "&&" || two == "||") {
        out = two;
        (void)a;
        (void)b;
        return true;
    }
    return false;
}

}  // namespace

Result<std::vector<Token>> lex(std::string_view source) {
    std::vector<Token> out;
    uint32_t line = 1, col = 1;
    size_t i = 0;

    auto advance = [&](size_t n = 1) {
        for (size_t k = 0; k < n && i < source.size(); ++k) {
            if (source[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
            ++i;
        }
    };

    while (i < source.size()) {
        const char c = source[i];
        if (c == '\n' || c == ' ' || c == '\t' || c == '\r') {
            advance();
            continue;
        }
        if (c == '#') {  // comment to end of line
            while (i < source.size() && source[i] != '\n') advance();
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            const size_t start = i;
            while (i < source.size() &&
                   std::isdigit(static_cast<unsigned char>(source[i]))) {
                advance();
            }
            Token t;
            t.kind = TokKind::Int;
            t.text = std::string(source.substr(start, i - start));
            t.int_value = std::strtoll(t.text.c_str(), nullptr, 10);
            t.line = line;
            t.column = col;
            out.push_back(std::move(t));
            continue;
        }
        if (is_ident_start(c)) {
            const size_t start = i;
            while (i < source.size() && is_ident_char(source[i])) advance();
            Token t;
            t.text = std::string(source.substr(start, i - start));
            t.kind = is_keyword(t.text) ? TokKind::Keyword : TokKind::Ident;
            t.line = line;
            t.column = col;
            out.push_back(std::move(t));
            continue;
        }
        std::string two;
        if (match_two_char(source, i, two)) {
            Token t;
            t.kind = TokKind::Punct;
            t.text = two;
            t.line = line;
            t.column = col;
            out.push_back(std::move(t));
            advance(2);
            continue;
        }
        static const char* kSingle = "+-*/%(){};,<>!=<>";
        if (std::string("+-*/%(){};,<>!=").find(c) != std::string::npos ||
            c == '<' || c == '>' || c == '!' || c == '=') {
            (void)kSingle;
            Token t;
            t.kind = TokKind::Punct;
            t.text = std::string(1, c);
            t.line = line;
            t.column = col;
            out.push_back(std::move(t));
            advance();
            continue;
        }
        return support::fail(support::ErrorCode::SyntaxError,
                             std::string("unexpected character '") + c + "'",
                             line, col);
    }

    Token end;
    end.kind = TokKind::End;
    end.line = line;
    end.column = col;
    out.push_back(std::move(end));
    return out;
}

}  // namespace vortex::frontends::reference
