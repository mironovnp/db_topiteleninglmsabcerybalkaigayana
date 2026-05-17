#pragma once
#include <string>
#include <openssl/evp.h>
#include <iomanip>
#include <sstream>

namespace db {

inline std::string hashPassword(const std::string& password) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    const bool ok =
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, password.data(), password.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, hash, &hash_len) == 1;

    EVP_MD_CTX_free(ctx);
    if (!ok) return {};

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

} // namespace db
