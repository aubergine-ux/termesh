#include "core/json.hpp"

#include <cmath>

#include <cstdlib>

namespace termesh {

const Json& Json::null() {
    static const Json value;
    return value;
}

const Json& Json::operator[](std::string_view key) const {
    for (const auto& member : members_)
        if (member.first == key) return member.second;
    return null();
}

class JsonParser {
public:
    JsonParser(std::string_view text, std::string& error) : text_(text), error_(error) {}

    bool document(Json& out) {
        if (!value(out, 0)) return false;
        skipSpace();
        if (pos_ != text_.size()) return fail("unexpected text after the end of the document");
        return true;
    }

private:
    // Deeply nested input would otherwise overflow the stack: each level of
    // nesting is a level of recursion.
    static constexpr int kMaxDepth = 64;

    bool fail(const std::string& message) {
        // Convert the position to a line and column for the message.
        std::size_t line = 1, column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') ++line, column = 1;
            else ++column;
        }
        error_ = "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message;
        return false;
    }

    void skipSpace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
                while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > kMaxDepth) return fail("nested too deeply");
        skipSpace();
        if (pos_ >= text_.size()) return fail("unexpected end of file");
        const char c = text_[pos_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.type_ = Json::Type::String;
            return string(out.string_);
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        if (literal("true")) {
            out.type_ = Json::Type::Bool;
            out.bool_ = true;
            return true;
        }
        if (literal("false")) {
            out.type_ = Json::Type::Bool;
            return true;
        }
        if (literal("null")) return true;
        return fail("expected a value");
    }

    bool object(Json& out, int depth) {
        out.type_ = Json::Type::Object;
        ++pos_;  // {
        while (true) {
            skipSpace();
            if (pos_ >= text_.size()) return fail("unexpected end of file inside an object");
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (text_[pos_] != '"') return fail("expected a member name in quotes");
            std::string key;
            if (!string(key)) return false;
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':' after the member name");
            ++pos_;
            Json member;
            if (!value(member, depth + 1)) return false;
            out.members_.emplace_back(std::move(key), std::move(member));
            skipSpace();
            if (pos_ >= text_.size()) return fail("unexpected end of file inside an object");
            if (text_[pos_] == ',') ++pos_;
            else if (text_[pos_] != '}') return fail("expected ',' or '}'");
        }
    }

    bool array(Json& out, int depth) {
        out.type_ = Json::Type::Array;
        ++pos_;  // [
        while (true) {
            skipSpace();
            if (pos_ >= text_.size()) return fail("unexpected end of file inside an array");
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            Json item;
            if (!value(item, depth + 1)) return false;
            out.items_.push_back(std::move(item));
            skipSpace();
            if (pos_ >= text_.size()) return fail("unexpected end of file inside an array");
            if (text_[pos_] == ',') ++pos_;
            else if (text_[pos_] != ']') return fail("expected ',' or ']'");
        }
    }

    bool number(Json& out) {
        // Find the end of the number by the JSON rules, then let the C
        // library do the conversion.
        const std::size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        auto digits = [&] {
            const std::size_t before = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            return pos_ > before;
        };
        if (!digits()) return fail("expected a digit");
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!digits()) return fail("expected a digit after the decimal point");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (!digits()) return fail("expected a digit in the exponent");
        }
        out.type_ = Json::Type::Number;
        out.number_ = std::strtod(std::string(text_.substr(start, pos_ - start)).c_str(), nullptr);
        // Every number in a scene file ends up in a float. One too large
        // for that would become infinity, and spread from there.
        if (!std::isfinite(out.number_) || std::abs(out.number_) > 1e30) return fail("the number is too large");
        return true;
    }

    static void appendUtf8(std::string& out, unsigned codepoint) {
        if (codepoint < 0x80) {
            out += static_cast<char>(codepoint);
        } else if (codepoint < 0x800) {
            out += static_cast<char>(0xC0 | (codepoint >> 6));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else if (codepoint < 0x10000) {
            out += static_cast<char>(0xE0 | (codepoint >> 12));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (codepoint >> 18));
            out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
    }

    bool hex4(unsigned& out) {
        if (pos_ + 4 > text_.size()) return fail("incomplete \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool string(std::string& out) {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= text_.size()) return fail("unterminated string");
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c == '\n') {
                --pos_;
                return fail("unterminated string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return fail("unterminated string");
            const char escape = text_[pos_++];
            switch (escape) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned codepoint = 0;
                    if (!hex4(codepoint)) return false;
                    // Characters beyond U+FFFF are written as two escapes,
                    // a "surrogate pair".
                    if (codepoint >= 0xD800 && codepoint < 0xDC00 && text_.substr(pos_, 2) == "\\u") {
                        pos_ += 2;
                        unsigned low = 0;
                        if (!hex4(low)) return false;
                        if (low >= 0xDC00 && low < 0xE000)
                            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                        else codepoint = 0xFFFD;
                    } else if (codepoint >= 0xD800 && codepoint < 0xE000) {
                        codepoint = 0xFFFD;  // half a pair on its own
                    }
                    appendUtf8(out, codepoint);
                    break;
                }
                default: --pos_; return fail("unknown escape in a string");
            }
        }
    }

    std::string_view text_;
    std::string& error_;
    std::size_t pos_ = 0;
};

bool Json::parse(std::string_view text, Json& out, std::string& error) {
    out = Json();
    return JsonParser(text, error).document(out);
}

}  // namespace termesh
