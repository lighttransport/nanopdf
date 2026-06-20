// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Simple cryptographic algorithms implementation for PDF security
// This is a basic implementation for educational purposes

#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <array>

namespace nanopdf {
namespace crypto {

// RC4 stream cipher
class RC4 {
public:
  RC4();
  void init(const uint8_t* key, size_t key_len);
  void crypt(uint8_t* data, size_t len);

private:
  uint8_t state[256];
  uint8_t x;
  uint8_t y;

  void swap(uint8_t& a, uint8_t& b);
};

// AES block cipher (128-bit)
class AES128 {
public:
  static const size_t BLOCK_SIZE = 16;
  static const size_t KEY_SIZE = 16;

  AES128();
  void set_key(const uint8_t* key);
  void encrypt_block(const uint8_t* in, uint8_t* out);
  void decrypt_block(const uint8_t* in, uint8_t* out);

  // CBC mode
  void encrypt_cbc(const uint8_t* in, uint8_t* out, size_t len, const uint8_t* iv);
  void decrypt_cbc(const uint8_t* in, uint8_t* out, size_t len, const uint8_t* iv);

private:
  uint8_t round_keys[11][16];  // 11 round keys for AES-128

  void key_expansion(const uint8_t* key);
  void add_round_key(uint8_t* state, const uint8_t* round_key);
  void sub_bytes(uint8_t* state);
  void inv_sub_bytes(uint8_t* state);
  void shift_rows(uint8_t* state);
  void inv_shift_rows(uint8_t* state);
  void mix_columns(uint8_t* state);
  void inv_mix_columns(uint8_t* state);

  uint8_t gmul(uint8_t a, uint8_t b);
};

// AES-256 block cipher
class AES256 {
public:
  static const size_t BLOCK_SIZE = 16;
  static const size_t KEY_SIZE = 32;

  AES256();
  void set_key(const uint8_t* key);
  void encrypt_block(const uint8_t* in, uint8_t* out);
  void decrypt_block(const uint8_t* in, uint8_t* out);

  // CBC mode
  void encrypt_cbc(const uint8_t* in, uint8_t* out, size_t len, const uint8_t* iv);
  void decrypt_cbc(const uint8_t* in, uint8_t* out, size_t len, const uint8_t* iv);

private:
  uint8_t round_keys[15][16];  // 15 round keys for AES-256

  void key_expansion(const uint8_t* key);
  void add_round_key(uint8_t* state, const uint8_t* round_key);
  void sub_bytes(uint8_t* state);
  void inv_sub_bytes(uint8_t* state);
  void shift_rows(uint8_t* state);
  void inv_shift_rows(uint8_t* state);
  void mix_columns(uint8_t* state);
  void inv_mix_columns(uint8_t* state);

  uint8_t gmul(uint8_t a, uint8_t b);
};

// MD5 hash function
class MD5 {
public:
  static const size_t DIGEST_SIZE = 16;

  MD5();
  void update(const uint8_t* data, size_t len);
  void finalize();
  void get_digest(uint8_t* digest) const;

  // Convenience function
  static void hash(const uint8_t* data, size_t len, uint8_t* digest);

private:
  uint32_t state[4];
  uint64_t count;
  uint8_t buffer[64];
  uint8_t digest[16];
  bool finalized;

  void transform(const uint8_t* block);
  static uint32_t f(uint32_t x, uint32_t y, uint32_t z);
  static uint32_t g(uint32_t x, uint32_t y, uint32_t z);
  static uint32_t h(uint32_t x, uint32_t y, uint32_t z);
  static uint32_t i(uint32_t x, uint32_t y, uint32_t z);
  static uint32_t rotate_left(uint32_t x, uint32_t n);
};

// SHA-256 hash function
class SHA256 {
public:
  static const size_t DIGEST_SIZE = 32;

  SHA256();
  void update(const uint8_t* data, size_t len);
  void finalize();
  void get_digest(uint8_t* digest) const;

  // Convenience function
  static void hash(const uint8_t* data, size_t len, uint8_t* digest);

private:
  uint32_t state[8];
  uint64_t count;
  uint8_t buffer[64];
  uint8_t digest[32];
  bool finalized;

  void transform(const uint8_t* block);
  static uint32_t ch(uint32_t x, uint32_t y, uint32_t z);
  static uint32_t maj(uint32_t x, uint32_t y, uint32_t z);
  static uint32_t ep0(uint32_t x);
  static uint32_t ep1(uint32_t x);
  static uint32_t sig0(uint32_t x);
  static uint32_t sig1(uint32_t x);
  static uint32_t rotr(uint32_t x, uint32_t n);
};

// SHA-1 hash function
class SHA1 {
public:
  static const size_t DIGEST_SIZE = 20;

  SHA1();
  void update(const uint8_t* data, size_t len);
  void finalize();
  void get_digest(uint8_t* digest) const;

  // Convenience function
  static void hash(const uint8_t* data, size_t len, uint8_t* digest);

private:
  uint32_t state[5];
  uint64_t count;
  uint8_t buffer[64];
  uint8_t digest[20];
  bool finalized;

  void transform(const uint8_t* block);
  static uint32_t rotl(uint32_t x, uint32_t n);
};

// SHA-512 hash function
class SHA512 {
public:
  static const size_t DIGEST_SIZE = 64;

  SHA512();
  void update(const uint8_t* data, size_t len);
  void finalize();
  void get_digest(uint8_t* digest) const;

  static void hash(const uint8_t* data, size_t len, uint8_t* digest);

private:
  uint64_t state[8];
  uint64_t count;
  uint8_t buffer[128];
  uint8_t digest[64];
  bool finalized;

  void transform(const uint8_t* block);
  static uint64_t rotr64(uint64_t x, uint64_t n);
  static uint64_t ch64(uint64_t x, uint64_t y, uint64_t z);
  static uint64_t maj64(uint64_t x, uint64_t y, uint64_t z);
  static uint64_t ep0_64(uint64_t x);
  static uint64_t ep1_64(uint64_t x);
  static uint64_t sig0_64(uint64_t x);
  static uint64_t sig1_64(uint64_t x);
};

// SHA-384 hash function (SHA-512 with different IV, truncated to 48 bytes)
class SHA384 {
public:
  static const size_t DIGEST_SIZE = 48;

  SHA384();
  void update(const uint8_t* data, size_t len);
  void finalize();
  void get_digest(uint8_t* digest) const;

  static void hash(const uint8_t* data, size_t len, uint8_t* digest);

private:
  uint64_t state[8];
  uint64_t count;
  uint8_t buffer[128];
  uint8_t digest[48];
  bool finalized;

  void transform(const uint8_t* block);
  static uint64_t rotr64(uint64_t x, uint64_t n);
  static uint64_t ch64(uint64_t x, uint64_t y, uint64_t z);
  static uint64_t maj64(uint64_t x, uint64_t y, uint64_t z);
  static uint64_t ep0_64(uint64_t x);
  static uint64_t ep1_64(uint64_t x);
  static uint64_t sig0_64(uint64_t x);
  static uint64_t sig1_64(uint64_t x);
};

// Utility functions
void xor_bytes(uint8_t* dest, const uint8_t* src, size_t len);
void pad_pkcs7(std::vector<uint8_t>& data, size_t block_size);
size_t unpad_pkcs7(uint8_t* data, size_t len);

// HMAC + PBKDF2 (used by PKCS#8/PBES2 and PKCS#12). PRF selector: 1=SHA-1,
// 2=SHA-256, 3=SHA-512.
enum class Prf { Sha1 = 1, Sha256 = 2, Sha512 = 3 };

std::vector<uint8_t> hmac(Prf prf, const uint8_t* key, size_t key_len,
                          const uint8_t* msg, size_t msg_len);
std::vector<uint8_t> pbkdf2(Prf prf, const uint8_t* password, size_t pw_len,
                            const uint8_t* salt, size_t salt_len, int iterations,
                            size_t dk_len);

} // namespace crypto
} // namespace nanopdf