#include "engine/cell_value.hpp"
#include "engine/storage/storage.hpp"
#include <cctype>

namespace db {

std::string cell_primitive_to_lexical_for_key(const CellPrimitive& p) {
    if (auto* pv = std::get_if<int64_t>(&p)) return std::to_string(*pv);
    if (auto* pv = std::get_if<double>(&p)) return std::to_string(*pv);
    if (auto* pv = std::get_if<bool>(&p)) return *pv ? "true" : "false";
    if (auto* pv = std::get_if<std::string>(&p)) return *pv;
    return {};
}

namespace {

int compare_primitives(const CellPrimitive& a, const CellPrimitive& b) {
    return std::visit(
        [&](const auto& va) {
            return std::visit(
                [&](const auto& vb) {
                    using TA = std::decay_t<decltype(va)>;
                    using TB = std::decay_t<decltype(vb)>;
                    if constexpr (std::is_same_v<TA, TB>) {
                        if (va < vb) return -1;
                        if (vb < va) return 1;
                        return 0;
                    }
                    return cell_primitive_to_lexical_for_key(CellPrimitive{va}).compare(
                        cell_primitive_to_lexical_for_key(CellPrimitive{vb}));
                },
                b);
        },
        a);
}

} // namespace

const uint8_t kRowMagic[4] = {'T', 'B', 'W', '2'};

int compare_cell_values(const CellValue& a, const CellValue& b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return compare_primitives(*a, *b);
}

int compare_btree_keys(const BTreeKey& a, const BTreeKey& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        int c = compare_cell_values(a[i], b[i]);
        if (c != 0) return c;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

int compare_btree_keys_nav(const BTreeKey& a, const BTreeKey& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        int c = compare_cell_values(a[i], b[i]);
        if (c != 0) return c;
    }
    if (a.size() == b.size()) return 0;
    // Prefix rule: shorter key neither less nor greater (matches legacy composite '\0' behaviour).
    return 0;
}

CellPrimitive parse_numeric_literal_strict(const ColumnDef& col,
                                              const std::string& literal,
                                              bool quoted) {
    const std::string& t = col.type;
    if (quoted || t == "TEXT")
        return std::string{literal};
    if (t == "INT") return static_cast<int64_t>(std::stol(literal));
    if (t == "FLOAT") return std::stod(literal);
    if (t == "BOOL") {
        std::string s = literal;
        for (auto& c : s) c = static_cast<char>(tolower(c));
        if (s == "true" || s == "1") return true;
        if (s == "false" || s == "0") return false;
        return std::string{literal};
    }
    return std::string{literal};
}

CellValue coerce_string_to_cell_column(const ColumnDef& col, const std::string& literal,
                                       bool literal_was_quoted) {
    if (!literal_was_quoted && literal == "NULL")
        return std::nullopt;
    try {
        if (literal_was_quoted || col.type == "TEXT") {
            std::string s = literal;
            if (literal_was_quoted && s.size() >= 2 &&
                ((s.front() == '\'' && s.back() == '\'') || (s.front() == '\"' && s.back() == '\"')))
                s = s.substr(1, s.size() - 2);
            return CellPrimitive{std::move(s)};
        }
        return parse_numeric_literal_strict(col, literal, false);
    } catch (...) {
        return CellPrimitive{std::string{literal}};
    }
}

Row legacy_strings_to_cells(const ColumnDef* columns, std::size_t ncols,
                            const std::vector<std::string>& legacy_strings) {
    Row out;
    out.reserve(ncols);
    for (std::size_t i = 0; i < ncols; ++i) {
        std::string s = i < legacy_strings.size() ? legacy_strings[i] : std::string{};
        if (!columns) {
            out.push_back(CellPrimitive{std::move(s)});
            continue;
        }
        const ColumnDef& cd = columns[i];
        // Legacy empties behaved as SQL NULL except possibly TEXT empty-string
        if (s.empty() && cd.type != "TEXT") {
            out.push_back(std::nullopt);
            continue;
        }
        if (s.empty() && cd.type == "TEXT") {
            out.push_back(CellPrimitive{std::string{}});
            continue;
        }
        out.push_back(coerce_string_to_cell_column(cd, s, false));
    }
    return out;
}

} // namespace db
