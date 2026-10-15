// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>

#include "common/types.h"

class Crypto {
public:
    void RSA2048Decrypt(std::span<u8, 32> dec_key, std::span<const u8, 256> ciphertext,
                        bool is_dk3);
    void ivKeyHASH256(std::span<const u8, 64> cipher_input, std::span<u8, 32> ivkey_result);
    void aesCbcCfb128Decrypt(std::span<const u8, 32> ivkey, std::span<const u8, 256> ciphertext,
                             std::span<u8, 256> decrypted);
    void aesCbcCfb128DecryptEntry(std::span<const u8, 32> ivkey, std::span<u8> ciphertext,
                                  std::span<u8> decrypted);
    void PfsGenCryptoKey(std::span<const u8, 32> ekpfs, std::span<const u8, 16> seed,
                         std::span<u8, 16> dataKey, std::span<u8, 16> tweakKey);
    void decryptPFS(std::span<const u8, 16> dataKey, std::span<const u8, 16> tweakKey,
                    std::span<const u8> src_image, std::span<u8> dst_image, u64 sector);

    static std::string HmacSha256Hex(std::span<const u8> key, std::span<const u8> message);

private:
    static void xtsXorBlock(u8* x, const u8* a, const u8* b) {
        for (int i = 0; i < 16; i++) {
            x[i] = a[i] ^ b[i];
        }
    }

    static void xtsMult(std::span<u8, 16> encryptedTweak) {
        int feedback = 0;
        for (size_t k = 0; k < encryptedTweak.size(); k++) {
            const auto tmp = (encryptedTweak[k] >> 7) & 1;
            encryptedTweak[k] = ((encryptedTweak[k] << 1) + feedback) & 0xFF;
            feedback = tmp;
        }
        if (feedback != 0) {
            encryptedTweak[0] ^= 0x87;
        }
    }
};
