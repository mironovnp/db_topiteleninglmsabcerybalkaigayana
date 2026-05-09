#include "engine/storage/storage.hpp"
#include <cstring>

namespace db {

std::string Storage::serializeSchema(const TableSchema& s) {
    std::string buf;

    uint32_t root = INVALID_PAGE_ID;
    buf.append(reinterpret_cast<const char*>(&root), 4);

    uint32_t pk = static_cast<uint32_t>(s.primary_key_index);
    buf.append(reinterpret_cast<const char*>(&pk), 4);

    uint32_t nc = static_cast<uint32_t>(s.columns.size());
    buf.append(reinterpret_cast<const char*>(&nc), 4);

    for (const auto& col : s.columns) {
        uint16_t nl = static_cast<uint16_t>(col.name.size());
        buf.append(reinterpret_cast<const char*>(&nl), 2);
        buf.append(col.name);
        uint16_t tl = static_cast<uint16_t>(col.type.size());
        buf.append(reinterpret_cast<const char*>(&tl), 2);
        buf.append(col.type);
        uint16_t flags = 0;
        if (col.not_null) flags |= 0x0001;
        if (col.unique) flags |= 0x0002;
        if (col.has_default) flags |= 0x0004;
        if (!col.fk_ref_table.empty()) flags |= 0x0008;
        if (col.on_delete == OnDeleteAction::CASCADE) flags |= 0x0010;
        else if (col.on_delete == OnDeleteAction::SET_NULL) flags |= 0x0020;
        if (col.is_autoincrement) flags |= 0x0040;
        if (col.on_update == OnUpdateAction::CASCADE) flags |= 0x0080;
        else if (col.on_update == OnUpdateAction::SET_NULL) flags |= 0x0100;
        buf.append(reinterpret_cast<const char*>(&flags), 2);
        if (col.has_default) {
            uint16_t dl = static_cast<uint16_t>(col.default_value.size());
            buf.append(reinterpret_cast<const char*>(&dl), 2);
            buf.append(col.default_value);
        }
        if (!col.fk_ref_table.empty()) {
            uint16_t trl = static_cast<uint16_t>(col.fk_ref_table.size());
            buf.append(reinterpret_cast<const char*>(&trl), 2);
            buf.append(col.fk_ref_table);
            uint16_t crl = static_cast<uint16_t>(col.fk_ref_column.size());
            buf.append(reinterpret_cast<const char*>(&crl), 2);
            buf.append(col.fk_ref_column);
        }
    }

    uint32_t ni = static_cast<uint32_t>(s.indexes.size());
    buf.append(reinterpret_cast<const char*>(&ni), 4);
    for (const auto& idx : s.indexes) {
        uint16_t inl = static_cast<uint16_t>(idx.index_name.size());
        buf.append(reinterpret_cast<const char*>(&inl), 2);
        buf.append(idx.index_name);
        uint16_t icl = static_cast<uint16_t>(idx.column_name.size());
        buf.append(reinterpret_cast<const char*>(&icl), 2);
        buf.append(idx.column_name);
    }

    return buf;
}

TableSchema Storage::deserializeSchema(const char* data, uint32_t len) {
    TableSchema s;
    const char* p = data;
    const char* end = data + len;

    p += 4;

    uint32_t pk;
    memcpy(&pk, p, 4);
    p += 4;
    s.primary_key_index = static_cast<int>(pk);

    uint32_t nc;
    memcpy(&nc, p, 4);
    p += 4;

    for (uint32_t i = 0; i < nc && p + 2 <= end; ++i) {
        ColumnDef cd;
        uint16_t nl;
        memcpy(&nl, p, 2);
        p += 2;
        if (p + nl > end) break;
        cd.name.assign(p, nl);
        p += nl;
        uint16_t tl;
        memcpy(&tl, p, 2);
        p += 2;
        if (p + tl > end) break;
        cd.type.assign(p, tl);
        p += tl;
        bool has_fk = false;
        if (p + 2 <= end) {
            uint16_t flags;
            memcpy(&flags, p, 2);
            p += 2;
            cd.not_null = (flags & 0x0001) != 0;
            cd.unique = (flags & 0x0002) != 0;
            cd.has_default = (flags & 0x0004) != 0;
            has_fk = (flags & 0x0008) != 0;
            if (flags & 0x0010) cd.on_delete = OnDeleteAction::CASCADE;
            else if (flags & 0x0020) cd.on_delete = OnDeleteAction::SET_NULL;
            else cd.on_delete = OnDeleteAction::NO_ACTION;
            cd.is_autoincrement = (flags & 0x0040) != 0;
            if (flags & 0x0080) cd.on_update = OnUpdateAction::CASCADE;
            else if (flags & 0x0100) cd.on_update = OnUpdateAction::SET_NULL;
            else cd.on_update = OnUpdateAction::NO_ACTION;
        }
        if (cd.has_default && p + 2 <= end) {
            uint16_t dl;
            memcpy(&dl, p, 2);
            p += 2;
            if (p + dl <= end) {
                cd.default_value.assign(p, dl);
                p += dl;
            }
        }
        if (has_fk && p + 2 <= end) {
            uint16_t trl;
            memcpy(&trl, p, 2);
            p += 2;
            if (p + trl <= end) {
                cd.fk_ref_table.assign(p, trl);
                p += trl;
            }
            if (p + 2 <= end) {
                uint16_t crl;
                memcpy(&crl, p, 2);
                p += 2;
                if (p + crl <= end) {
                    cd.fk_ref_column.assign(p, crl);
                    p += crl;
                }
            }
        }
        s.columns.push_back(std::move(cd));
    }

    if (p + 4 <= end) {
        uint32_t ni;
        memcpy(&ni, p, 4);
        p += 4;
        for (uint32_t i = 0; i < ni && p + 2 <= end; ++i) {
            IndexDef idx;
            uint16_t inl;
            memcpy(&inl, p, 2);
            p += 2;
            if (p + inl > end) break;
            idx.index_name.assign(p, inl);
            p += inl;
            if (p + 2 > end) break;
            uint16_t icl;
            memcpy(&icl, p, 2);
            p += 2;
            if (p + icl > end) break;
            idx.column_name.assign(p, icl);
            p += icl;
            s.indexes.push_back(std::move(idx));
        }
    }

    return s;
}

} // namespace db
