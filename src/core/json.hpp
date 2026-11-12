// Json: a small JSON reader, for scene files.
//
// A recursive-descent parser: one function per kind of value, each of which
// looks at the next character to decide what it is reading and calls the
// others for whatever is nested inside. The grammar is small enough that
// the code is the grammar.
//
// Two liberties are taken with the standard, because scene files are
// written by hand: `//` comments run to the end of the line, and a comma
// may follow the last element of an array or object.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace termesh {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;

    // Parse a whole document. On failure returns false and describes the
    // problem, with its line and column, in `error`.
    static bool parse(std::string_view text, Json& out, std::string& error);

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // The value, or `fallback` if this is not of that type. Reading a
    // setting that was left out of a file is therefore one call:
    // json["fov"].number(40).
    bool boolean(bool fallback = false) const { return isBool() ? bool_ : fallback; }
    double number(double fallback = 0.0) const { return isNumber() ? number_ : fallback; }
    const std::string& string() const { return string_; }  // empty unless a string
    std::string string(const std::string& fallback) const { return isString() ? string_ : fallback; }

    // Arrays.
    std::size_t size() const { return items_.size(); }
    const std::vector<Json>& items() const { return items_; }
    const Json& operator[](std::size_t index) const { return index < items_.size() ? items_[index] : null(); }
    // (A literal 0 would otherwise be taken for a null string as readily
    // as for an index.)
    const Json& operator[](int index) const { return index >= 0 ? (*this)[static_cast<std::size_t>(index)] : null(); }

    // Objects. Members keep the order they had in the file. Looking up a
    // key that is not there gives a null value rather than an error, so
    // lookups can be chained: json["camera"]["path"]["keys"].
    const std::vector<std::pair<std::string, Json>>& members() const { return members_; }
    const Json& operator[](std::string_view key) const;
    const Json& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    bool has(std::string_view key) const { return !(*this)[key].isNull(); }

private:
    friend class JsonParser;
    static const Json& null();

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

}  // namespace termesh
