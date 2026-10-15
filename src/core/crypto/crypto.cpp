// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include <openssl/aes.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include "common/logging/log.h"
#include "core/crypto/crypto.h"
#include "core/crypto/keys.h"

namespace {

struct RsaDeleter {
    void operator()(RSA* rsa) const {
        RSA_free(rsa);
    }
};
using RsaPtr = std::unique_ptr<RSA, RsaDeleter>;

template <typename Keyset>
RsaPtr MakePrivateKey() {
    RsaPtr rsa{RSA_new()};
    if (!rsa) {
        return nullptr;
    }
    const auto bn = [](const auto& bytes) {
        return BN_bin2bn(bytes, static_cast<int>(sizeof(bytes)), nullptr);
    };
    BIGNUM* n = bn(Keyset::Modulus);
    BIGNUM* e = bn(Keyset::PublicExponent);
    BIGNUM* d = bn(Keyset::PrivateExponent);
    BIGNUM* p = bn(Keyset::Prime1);
    BIGNUM* q = bn(Keyset::Prime2);
    BIGNUM* dmp1 = bn(Keyset::Exponent1);
    BIGNUM* dmq1 = bn(Keyset::Exponent2);
    BIGNUM* iqmp = bn(Keyset::Coefficient);

    if (RSA_set0_key(rsa.get(), n, e, d) != 1) {
        BN_free(n), BN_free(e), BN_free(d);
        BN_free(p), BN_free(q), BN_free(dmp1), BN_free(dmq1), BN_free(iqmp);
        return nullptr;
    }
    if (RSA_set0_factors(rsa.get(), p, q) != 1) {
        BN_free(p), BN_free(q), BN_free(dmp1), BN_free(dmq1), BN_free(iqmp);
        return nullptr;
    }
    if (RSA_set0_crt_params(rsa.get(), dmp1, dmq1, iqmp) != 1) {
        BN_free(dmp1), BN_free(dmq1), BN_free(iqmp);
        return nullptr;
    }
    return rsa;
}

void AesCbcDecrypt(std::span<const u8, 32> ivkey, const u8* in, u8* out, size_t size) {
    AES_KEY key;
    AES_set_decrypt_key(ivkey.data() + 16, 128, &key);
    std::array<u8, 16> iv;
    std::memcpy(iv.data(), ivkey.data(), iv.size());

    const size_t aligned = size & ~size_t{15};
    if (aligned != 0) {
        AES_cbc_encrypt(in, out, aligned, &key, iv.data(), AES_DECRYPT);
    }
    if (aligned != size && in != out) {
        std::memcpy(out + aligned, in + aligned, size - aligned);
    }
}

std::string ToHex(const u8* data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; i++) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0xF]);
    }
    return out;
}

} // namespace

void Crypto::RSA2048Decrypt(std::span<u8, 32> dec_key, std::span<const u8, 256> ciphertext,
                            bool is_dk3) {
    std::fill(dec_key.begin(), dec_key.end(), u8{0});
    const RsaPtr rsa =
        is_dk3 ? MakePrivateKey<PkgDerivedKey3Keyset>() : MakePrivateKey<FakeKeyset>();
    if (!rsa) {
        LOG_ERROR(Loader, "Failed to build RSA key for PKG decryption");
        return;
    }

    std::array<u8, 256> decrypted{};
    const int len = RSA_private_decrypt(static_cast<int>(ciphertext.size()), ciphertext.data(),
                                        decrypted.data(), rsa.get(), RSA_PKCS1_PADDING);
    if (len < 0) {
        LOG_ERROR(Loader, "RSA decryption of PKG key failed. This PKG is probably not a fake PKG.");
        return;
    }
    std::copy_n(decrypted.begin(), std::min<size_t>(dec_key.size(), static_cast<size_t>(len)),
                dec_key.begin());
}

void Crypto::ivKeyHASH256(std::span<const u8, 64> cipher_input, std::span<u8, 32> ivkey_result) {
    SHA256(cipher_input.data(), cipher_input.size(), ivkey_result.data());
}

void Crypto::aesCbcCfb128Decrypt(std::span<const u8, 32> ivkey, std::span<const u8, 256> ciphertext,
                                 std::span<u8, 256> decrypted) {
    AesCbcDecrypt(ivkey, ciphertext.data(), decrypted.data(), decrypted.size());
}

void Crypto::aesCbcCfb128DecryptEntry(std::span<const u8, 32> ivkey, std::span<u8> ciphertext,
                                      std::span<u8> decrypted) {
    AesCbcDecrypt(ivkey, ciphertext.data(), decrypted.data(),
                  std::min(ciphertext.size(), decrypted.size()));
}

void Crypto::PfsGenCryptoKey(std::span<const u8, 32> ekpfs, std::span<const u8, 16> seed,
                             std::span<u8, 16> dataKey, std::span<u8, 16> tweakKey) {
    std::array<u8, 20> d{};
    const u32 index = 1;
    std::memcpy(d.data(), &index, sizeof(index));
    std::memcpy(d.data() + sizeof(index), seed.data(), seed.size());

    std::array<u8, 32> digest{};
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(), ekpfs.data(), static_cast<int>(ekpfs.size()), d.data(), d.size(),
         digest.data(), &digest_len);

    std::copy_n(digest.begin(), tweakKey.size(), tweakKey.begin());
    std::copy_n(digest.begin() + tweakKey.size(), dataKey.size(), dataKey.begin());
}

void Crypto::decryptPFS(std::span<const u8, 16> dataKey, std::span<const u8, 16> tweakKey,
                        std::span<const u8> src_image, std::span<u8> dst_image, u64 sector) {
    AES_KEY tweak_enc;
    AES_KEY data_dec;
    AES_set_encrypt_key(tweakKey.data(), 128, &tweak_enc);
    AES_set_decrypt_key(dataKey.data(), 128, &data_dec);

    const size_t size = std::min(src_image.size(), dst_image.size()) & ~size_t{0xFFF};
    for (size_t i = 0; i < size; i += 0x1000) {
        const u64 current_sector = sector + (i / 0x1000);

        std::array<u8, 16> tweak{};
        std::array<u8, 16> encryptedTweak{};
        std::array<u8, 16> xorBuffer{};
        std::memcpy(tweak.data(), &current_sector, sizeof(u64));
        AES_encrypt(tweak.data(), encryptedTweak.data(), &tweak_enc);

        for (size_t offset = 0; offset < 0x1000; offset += 16) {
            xtsXorBlock(xorBuffer.data(), src_image.data() + i + offset, encryptedTweak.data());
            AES_decrypt(xorBuffer.data(), xorBuffer.data(), &data_dec);
            xtsXorBlock(dst_image.data() + i + offset, xorBuffer.data(), encryptedTweak.data());
            xtsMult(encryptedTweak);
        }
    }
}

std::string Crypto::HmacSha256Hex(std::span<const u8> key, std::span<const u8> message) {
    std::array<u8, 32> digest{};
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), message.data(), message.size(),
         digest.data(), &digest_len);
    return ToHex(digest.data(), digest_len);
}
