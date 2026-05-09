#include "engine/row_codec.hpp"
#include "engine/storage/storage.hpp"
#include <cstring>

namespace db {

namespace {

void append_u32(std::string& b, uint32_t v) {
    b.append(reinterpret_cast<const char*>(&v), 4);
}

void append_u16(std::string& b, uint16_t v) {
    b.append(reinterpret_cast<const char*>(&v), 2);
}

bool magic_is_v2(const uint8_t* p, uint32_t len) {
    return len >= sizeof(kRowMagic) && std::memcmp(p, kRowMagic, sizeof(kRowMagic)) == 0;
}

bool read_legacy_string_blob(const ColumnDef* columns, uint32_t ncols,
                              const uint8_t* blob, uint32_t len, Row& out) {
    std::vector<std::string> strings;
    const char* p = reinterpret_cast<const char*>(blob);
    const char* end = p + len;
    if (p + 2 > end) return false;
    uint16_t nf;
    std::memcpy(&nf, p, 2);
    p += 2;
    for (uint16_t i = 0; i < nf && p + 2 <= end; ++i) {
        uint16_t fl;
        std::memcpy(&fl, p, 2);
        p += 2;
        if (p + fl > end) return false;
        strings.emplace_back(p, fl);
        p += fl;
    }
    out = legacy_strings_to_cells(columns, static_cast<size_t>(ncols), strings);
    return true;
}

void wire_append_cell(std::string& b, CellValue cv) {
    if (!cv) {
        b.push_back(0);
        return;
    }
    std::visit(
        [&](const auto& prim) {
            using T = std::decay_t<decltype(prim)>;
            if constexpr (std::is_same_v<T, int64_t>) {
                b.push_back(static_cast<char>(StoredCellTag::Int));
                int64_t v = prim;
                b.append(reinterpret_cast<const char*>(&v), sizeof(int64_t));
            } else if constexpr (std::is_same_v<T, double>) {
                b.push_back(static_cast<char>(StoredCellTag::Float));
                double dv = prim;
                b.append(reinterpret_cast<const char*>(&dv), sizeof(double));
            } else if constexpr (std::is_same_v<T, bool>) {
                b.push_back(static_cast<char>(StoredCellTag::Bool));
                uint8_t u = prim ? 1 : 0;
                b.push_back(static_cast<char>(u));
            } else {
                b.push_back(static_cast<char>(StoredCellTag::Text));
                const std::string& s = prim;
                uint32_t sl = static_cast<uint32_t>(s.size());
                append_u32(b, sl);
                b.append(s.data(), s.size());
            }
        },
        *cv);
}

bool wire_read_cell(const uint8_t*& p, const uint8_t* end, CellValue& out) {
    if (p + 1 > end) return false;
    uint8_t tag = *p++;
    if (tag == 0) {
        out = std::nullopt;
        return true;
    }
    switch (static_cast<StoredCellTag>(tag)) {
    case StoredCellTag::Int: {
        if (p + sizeof(int64_t) > end) return false;
        int64_t v;
        std::memcpy(&v, p, sizeof(v));
        p += sizeof(v);
        out = CellPrimitive{v};
        return true;
    }
    case StoredCellTag::Float: {
        if (p + sizeof(double) > end) return false;
        double v;
        std::memcpy(&v, p, sizeof(v));
        p += sizeof(v);
        out = CellPrimitive{v};
        return true;
    }
    case StoredCellTag::Bool: {
        if (p + 1 > end) return false;
        uint8_t u = *p++;
        out = CellPrimitive{static_cast<bool>(u)};
        return true;
    }
    case StoredCellTag::Text: {
        if (p + 4 > end) return false;
        uint32_t sl;
        std::memcpy(&sl, p, 4);
        p += 4;
        if (p + sl > end) return false;
        out = CellPrimitive{
            std::string(reinterpret_cast<const char*>(p), sl)};
        p += sl;
        return true;
    }
    default:
        return false;
    }
}

} // namespace

void btree_append_cell(std::string& buf, CellValue cv) {
    wire_append_cell(buf, cv);
}

bool btree_read_cell(const uint8_t*& p, const uint8_t* end, CellValue& out) {
    return wire_read_cell(p, end, out);
}

std::string serialize_row_disk(const TableSchema& schema, const Row& row) {
    std::string buf;
    buf.reserve(row.size() * 32);
    buf.append(reinterpret_cast<const char*>(kRowMagic), sizeof(kRowMagic));
    uint16_t ncol = static_cast<uint16_t>(schema.columns.size());
    append_u16(buf, ncol);
    for (uint16_t i = 0; i < ncol; ++i) {
        CellValue cv =
            i < row.size() ? row[i] : std::nullopt;
        wire_append_cell(buf, cv);
    }
    return buf;
}

bool deserialize_row_disk(const TableSchema& schema, const uint8_t* blob,
                          uint32_t len, Row& out) {
    out.clear();
    if (!blob || len < sizeof(kRowMagic)) return false;

    const ColumnDef* colptr =
        schema.columns.empty() ? nullptr : schema.columns.data();

    if (!magic_is_v2(blob, len))
        return read_legacy_string_blob(colptr,
                                       static_cast<uint32_t>(schema.columns.size()),
                                       blob, len, out);

    const uint8_t* p = blob + sizeof(kRowMagic);
    const uint8_t* end_all = blob + len;
    if (p + 2 > end_all) return false;
    uint16_t ncol_b;
    std::memcpy(&ncol_b, p, 2);
    p += 2;

    Row row;
    for (uint16_t i = 0; i < ncol_b && p < end_all; ++i) {
        CellValue cv;
        if (!wire_read_cell(p, end_all, cv)) {
            return read_legacy_string_blob(colptr,
                                           static_cast<uint32_t>(schema.columns.size()),
                                           blob, len, out);
        }
        row.push_back(std::move(cv));
    }

    while (row.size() < schema.columns.size())
        row.push_back(std::nullopt);

    out = std::move(row);
    return true;
}

void btree_key_append_bytes(std::string& buf, const BTreeKey& key) {
    for (const auto& c : key)
        wire_append_cell(buf, c);
}

bool btree_key_parts_from_bytes(const uint8_t* data, size_t len, uint8_t arity, BTreeKey& out) {
    const uint8_t* p = data;
    const uint8_t* end = data + len;
    out.clear();
    for (uint8_t i = 0; i < arity; ++i) {
        CellValue cv;
        if (!wire_read_cell(p, end, cv))
            return false;
        out.push_back(std::move(cv));
    }
    return p == end;
}

bool btree_key_from_legacy_bytes(const uint8_t* data, size_t len, uint8_t arity, const TableSchema& sch,
                                 BTreeKey& out) {
    out.clear();
    std::string raw(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + len);
    if (arity == 1) {
        if (sch.columns.empty() || sch.primary_key_index < 0 ||
            sch.primary_key_index >= static_cast<int>(sch.columns.size()))
            return false;
        out.push_back(
            coerce_string_to_cell_column(sch.columns[static_cast<size_t>(sch.primary_key_index)], raw,
                                         false));
        return true;
    }
    if (arity != 2 || sch.columns.size() < 2)
        return false;
    size_t z = raw.find('\0');
    if (z == std::string::npos)
        return false;
    std::string p0 = raw.substr(0, z);
    std::string p1 = raw.substr(z + 1);
    out.push_back(coerce_string_to_cell_column(sch.columns[0], p0, false));
    out.push_back(coerce_string_to_cell_column(sch.columns[1], p1, false));
    return true;
}

bool decode_btree_key_blob(const uint8_t* data, uint32_t len, uint8_t arity, const TableSchema& sch,
                           BTreeKey& out) {
    if (btree_key_parts_from_bytes(data, len, arity, out))
        return true;
    return btree_key_from_legacy_bytes(data, len, arity, sch, out);
}

} // namespace db
