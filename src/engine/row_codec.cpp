#include "engine/row_codec.hpp"
#include "engine/storage.hpp"
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

void append_payload(std::string& b, CellValue cv) {
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

} // namespace

std::string serialize_row_disk(const TableSchema& schema, const Row& row) {
    std::string buf;
    buf.reserve(row.size() * 32);
    buf.append(reinterpret_cast<const char*>(kRowMagic), sizeof(kRowMagic));
    uint16_t ncol = static_cast<uint16_t>(schema.columns.size());
    append_u16(buf, ncol);
    for (uint16_t i = 0; i < ncol; ++i) {
        CellValue cv =
            i < row.size() ? row[i] : std::nullopt;
        append_payload(buf, cv);
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
        if (p + 1 > end_all) return false;
        uint8_t tag = *p++;
        if (tag == 0) {
            row.push_back(std::nullopt);
            continue;
        }

        switch (static_cast<StoredCellTag>(tag)) {
        case StoredCellTag::Int: {
            if (p + sizeof(int64_t) > end_all) return false;
            int64_t v;
            std::memcpy(&v, p, sizeof(v));
            p += sizeof(v);
            row.push_back(CellPrimitive{v});
            break;
        }
        case StoredCellTag::Float: {
            if (p + sizeof(double) > end_all) return false;
            double v;
            std::memcpy(&v, p, sizeof(v));
            p += sizeof(v);
            row.push_back(CellPrimitive{v});
            break;
        }
        case StoredCellTag::Bool: {
            if (p + 1 > end_all) return false;
            uint8_t u = *p++;
            row.push_back(CellPrimitive{(bool)u});
            break;
        }
        case StoredCellTag::Text: {
            if (p + 4 > end_all) return false;
            uint32_t sl;
            std::memcpy(&sl, p, 4);
            p += 4;
            if (p + sl > end_all) return false;
            row.emplace_back(
                CellPrimitive{std::string(reinterpret_cast<const char*>(p), sl)});
            p += sl;
            break;
        }
        default:
            return read_legacy_string_blob(colptr,
                                           static_cast<uint32_t>(schema.columns.size()),
                                           blob, len, out);
        }
    }

    while (row.size() < schema.columns.size())
        row.push_back(std::nullopt);

    out = std::move(row);
    return true;
}

} // namespace db
