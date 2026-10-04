#include "password.h"
#include "Extension/Multiplayer/Net/protocol.h"
#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#else
#include <cstring>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#endif
#include <stdexcept>
#include <vector>
namespace dingosdk::multiplayer {
#ifdef _WIN32
namespace {
void check(NTSTATUS result) {
    if (result < 0)
        throw std::runtime_error("Lobby password cryptography unavailable.");
}
struct Algorithm {
    BCRYPT_ALG_HANDLE handle{};
    Algorithm() {
        check(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr,
                                          BCRYPT_ALG_HANDLE_HMAC_FLAG));
    }
    ~Algorithm() {
        if (handle)
            BCryptCloseAlgorithmProvider(handle, 0);
    }
};
} // namespace
#else
namespace {
void secure_zero(void *data, std::size_t size) noexcept {
    volatile unsigned char *p = static_cast<volatile unsigned char *>(data);
    while (size--) *p++ = 0;
}
} // namespace
#endif
std::optional<PasswordKey> password_key(std::string_view password, std::uint64_t session) {
    if (password.empty())
        return {};
    if (password.size() > 64 || !session)
        throw std::runtime_error("Lobby passwords must be 1-64 UTF-8 bytes.");
    std::array<std::uint8_t, 16> salt{'R', 'e', 'S', 'k',
                                      'a', 't', 'e', static_cast<std::uint8_t>(protocol_version)};
    for (unsigned i = 0; i < 8; ++i)
        salt[8 + i] = static_cast<std::uint8_t>(session >> (i * 8));
    PasswordKey key;
#ifdef _WIN32
    Algorithm algorithm;
    check(BCryptDeriveKeyPBKDF2(
        algorithm.handle, reinterpret_cast<PUCHAR>(const_cast<char *>(password.data())),
        static_cast<ULONG>(password.size()), salt.data(), static_cast<ULONG>(salt.size()), 100000, key.data(),
        static_cast<ULONG>(key.size()), 0));
#else
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                           static_cast<int>(salt.size()), 100000, EVP_sha256(),
                           static_cast<int>(key.size()), key.data()) != 1)
        throw std::runtime_error("Lobby password cryptography unavailable.");
#endif
    return key;
}
PasswordKey password_proof(const PasswordKey &key, std::uint64_t session, std::uint64_t map,
                           std::uint64_t host, std::uint64_t guest, std::uint64_t host_epoch,
                           std::uint64_t guest_epoch, std::uint64_t challenge) {
    std::vector<std::uint8_t> message{'R',
                                      'e',
                                      'S',
                                      'k',
                                      'a',
                                      't',
                                      'e',
                                      'P',
                                      'r',
                                      'o',
                                      'o',
                                      'f',
                                      static_cast<std::uint8_t>(protocol_version)};
    for (const auto v : {session, map, host, guest, host_epoch, guest_epoch, challenge})
        for (unsigned i = 0; i < 8; ++i)
            message.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
#ifdef _WIN32
    Algorithm algorithm;
    struct Hash {
        BCRYPT_HASH_HANDLE handle{};
        ~Hash() {
            if (handle)
                BCryptDestroyHash(handle);
        }
    } hash;
    check(BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, const_cast<PUCHAR>(key.data()),
                           static_cast<ULONG>(key.size()), 0));
    check(BCryptHashData(hash.handle, message.data(), static_cast<ULONG>(message.size()), 0));
    PasswordKey proof;
    check(BCryptFinishHash(hash.handle, proof.data(), static_cast<ULONG>(proof.size()), 0));
    return proof;
#else
    PasswordKey proof;
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), message.data(), message.size(),
              proof.data(), &length) ||
        length != proof.size())
        throw std::runtime_error("Lobby password cryptography unavailable.");
    return proof;
#endif
}
bool proof_matches(const PasswordKey &a, const PasswordKey &b) noexcept {
    volatile unsigned difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        difference = difference | (a[i] ^ b[i]);
    return difference == 0;
}
void erase_password(std::string &text) noexcept {
    if (!text.empty())
#ifdef _WIN32
        SecureZeroMemory(text.data(), text.size());
#else
        secure_zero(text.data(), text.size());
#endif
    text.clear();
}
void erase_key(std::optional<PasswordKey> &key) noexcept {
    if (key)
#ifdef _WIN32
        SecureZeroMemory(key->data(), key->size());
#else
        secure_zero(key->data(), key->size());
#endif
    key.reset();
}
} // namespace dingosdk::multiplayer
