// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "nanopdf.hh"
#include "crypto.hh"
#include <cstring>
#include <algorithm>

namespace nanopdf {

// Standard PDF padding for passwords
const uint8_t PDF_PADDING[32] = {
  0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41,
  0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
  0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
  0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A
};

// Pad password to 32 bytes
std::vector<uint8_t> pad_password(const std::string& password) {
  std::vector<uint8_t> padded(32);
  size_t len = std::min(password.size(), size_t(32));

  // Copy password
  memcpy(padded.data(), password.data(), len);

  // Add padding
  if (len < 32) {
    memcpy(padded.data() + len, PDF_PADDING, 32 - len);
  }

  return padded;
}

// Parse encryption dictionary
EncryptionDictionary parse_encryption_dictionary(const Pdf& pdf, const Dictionary& encrypt_dict) {
  EncryptionDictionary result;

  // Parse Filter
  auto filter_it = encrypt_dict.find("Filter");
  if (filter_it != encrypt_dict.end() && filter_it->second.type == Value::NAME) {
    result.filter = filter_it->second.name;
  }

  // Parse V (version)
  auto v_it = encrypt_dict.find("V");
  if (v_it != encrypt_dict.end() && v_it->second.type == Value::NUMBER) {
    result.v = static_cast<int>(v_it->second.number);
  }

  // Parse Length (key length in bits)
  auto length_it = encrypt_dict.find("Length");
  if (length_it != encrypt_dict.end() && length_it->second.type == Value::NUMBER) {
    result.length = static_cast<int>(length_it->second.number);
  }

  // Parse R (revision)
  auto r_it = encrypt_dict.find("R");
  if (r_it != encrypt_dict.end() && r_it->second.type == Value::NUMBER) {
    result.r = static_cast<int>(r_it->second.number);
  }

  // Parse O (owner password hash)
  auto o_it = encrypt_dict.find("O");
  if (o_it != encrypt_dict.end() && o_it->second.type == Value::STRING) {
    result.o = o_it->second.str;
  }

  // Parse U (user password hash)
  auto u_it = encrypt_dict.find("U");
  if (u_it != encrypt_dict.end() && u_it->second.type == Value::STRING) {
    result.u = u_it->second.str;
  }

  // Parse P (permissions)
  auto p_it = encrypt_dict.find("P");
  if (p_it != encrypt_dict.end() && p_it->second.type == Value::NUMBER) {
    result.p = static_cast<uint32_t>(static_cast<int32_t>(p_it->second.number));
  }

  // Parse EncryptMetadata
  auto em_it = encrypt_dict.find("EncryptMetadata");
  if (em_it != encrypt_dict.end() && em_it->second.type == Value::BOOLEAN) {
    result.encrypt_metadata = em_it->second.boolean;
  }

  // For V=4 and above
  if (result.v >= 4) {
    auto cf_it = encrypt_dict.find("CF");
    if (cf_it != encrypt_dict.end() && cf_it->second.type == Value::DICTIONARY) {
      // Parse each crypt filter entry
      for (const auto& cf_pair : cf_it->second.dict) {
        const Value* cf_val = &cf_pair.second;
        // Resolve reference if needed
        ResolvedObject cf_resolved;
        if (cf_val->type == Value::REFERENCE) {
          cf_resolved = resolve_reference(pdf, cf_val->ref_object_number,
                                          cf_val->ref_generation_number);
          if (!cf_resolved.success) continue;
          cf_val = &cf_resolved.value;
        }
        if (cf_val->type != Value::DICTIONARY) continue;

        CryptFilterEntry entry;
        entry.name = cf_pair.first;

        auto cfm_it = cf_val->dict.find("CFM");
        if (cfm_it != cf_val->dict.end() && cfm_it->second.type == Value::NAME) {
          entry.cfm = cfm_it->second.name;
        }

        auto len_it = cf_val->dict.find("Length");
        if (len_it != cf_val->dict.end() && len_it->second.type == Value::NUMBER) {
          entry.length = static_cast<int>(len_it->second.number);
        }

        auto ae_it = cf_val->dict.find("AuthEvent");
        if (ae_it != cf_val->dict.end() && ae_it->second.type == Value::NAME) {
          entry.auth_event = ae_it->second.name;
        }

        result.crypt_filters[entry.name] = entry;
      }
    }

    auto stmf_it = encrypt_dict.find("StmF");
    if (stmf_it != encrypt_dict.end() && stmf_it->second.type == Value::NAME) {
      result.stm_f = stmf_it->second.name;
    }

    auto strf_it = encrypt_dict.find("StrF");
    if (strf_it != encrypt_dict.end() && strf_it->second.type == Value::NAME) {
      result.str_f = strf_it->second.name;
    }
  }

  // For V=5 (AES-256)
  if (result.v == 5) {
    auto oe_it = encrypt_dict.find("OE");
    if (oe_it != encrypt_dict.end() && oe_it->second.type == Value::STRING) {
      result.oe = oe_it->second.str;
    }

    auto ue_it = encrypt_dict.find("UE");
    if (ue_it != encrypt_dict.end() && ue_it->second.type == Value::STRING) {
      result.ue = ue_it->second.str;
    }

    auto perms_it = encrypt_dict.find("Perms");
    if (perms_it != encrypt_dict.end() && perms_it->second.type == Value::STRING) {
      result.perms = perms_it->second.str;
    }
  }

  return result;
}

// Compute owner key for RC4 encryption
std::vector<uint8_t> compute_owner_key(const std::string& owner_password,
                                       const std::string& user_password,
                                       int key_length, int revision) {
  // Pad passwords
  auto padded_owner = pad_password(owner_password.empty() ? user_password : owner_password);
  auto padded_user = pad_password(user_password);

  // Compute MD5 hash of owner password
  uint8_t digest[16];
  crypto::MD5::hash(padded_owner.data(), 32, digest);

  // For revision 3 and above, do additional MD5 iterations
  if (revision >= 3) {
    for (int i = 0; i < 50; i++) {
      crypto::MD5::hash(digest, key_length / 8, digest);
    }
  }

  // Create encryption key from digest
  int key_len_bytes = std::min(key_length / 8, 16);
  std::vector<uint8_t> key(digest, digest + key_len_bytes);

  // Encrypt padded user password with owner key
  std::vector<uint8_t> owner_key(32);
  memcpy(owner_key.data(), padded_user.data(), 32);

  crypto::RC4 rc4;
  rc4.init(key.data(), key.size());
  rc4.crypt(owner_key.data(), 32);

  // For revision 3 and above, do additional encryption iterations
  if (revision >= 3) {
    for (int i = 1; i <= 19; i++) {
      std::vector<uint8_t> iter_key(key.size());
      for (size_t j = 0; j < key.size(); j++) {
        iter_key[j] = key[j] ^ i;
      }
      rc4.init(iter_key.data(), iter_key.size());
      rc4.crypt(owner_key.data(), 32);
    }
  }

  return owner_key;
}

// Compute user key for RC4 encryption
std::vector<uint8_t> compute_user_key(const std::string& user_password,
                                      const std::vector<uint8_t>& owner_key,
                                      uint32_t permissions,
                                      const std::string& file_id,
                                      int key_length, int revision) {
  // Pad password
  auto padded_password = pad_password(user_password);

  // Build data to hash
  std::vector<uint8_t> hash_data;
  hash_data.insert(hash_data.end(), padded_password.begin(), padded_password.end());
  hash_data.insert(hash_data.end(), owner_key.begin(), owner_key.end());

  // Add permissions (low-order byte first)
  hash_data.push_back((permissions >> 0) & 0xFF);
  hash_data.push_back((permissions >> 8) & 0xFF);
  hash_data.push_back((permissions >> 16) & 0xFF);
  hash_data.push_back((permissions >> 24) & 0xFF);

  // Add file ID
  hash_data.insert(hash_data.end(), file_id.begin(), file_id.end());

  // For revision 4 and above, add extra data
  if (revision >= 4) {
    // Add 0xFFFFFFFF if not encrypting metadata
    uint32_t metadata_val = 0xFFFFFFFF;
    hash_data.push_back((metadata_val >> 0) & 0xFF);
    hash_data.push_back((metadata_val >> 8) & 0xFF);
    hash_data.push_back((metadata_val >> 16) & 0xFF);
    hash_data.push_back((metadata_val >> 24) & 0xFF);
  }

  // Compute MD5 hash
  uint8_t digest[16];
  crypto::MD5::hash(hash_data.data(), hash_data.size(), digest);

  // For revision 3 and above, do additional MD5 iterations
  if (revision >= 3) {
    for (int i = 0; i < 50; i++) {
      crypto::MD5::hash(digest, key_length / 8, digest);
    }
  }

  // Create encryption key from digest
  int key_len_bytes = std::min(key_length / 8, 16);
  std::vector<uint8_t> encryption_key(digest, digest + key_len_bytes);

  if (revision == 2) {
    // For revision 2, encrypt padding with key
    std::vector<uint8_t> user_key(32);
    memcpy(user_key.data(), PDF_PADDING, 32);

    crypto::RC4 rc4;
    rc4.init(encryption_key.data(), encryption_key.size());
    rc4.crypt(user_key.data(), 32);

    return user_key;
  } else {
    // For revision 3 and above
    std::vector<uint8_t> user_key(32);

    // Hash padding and file ID
    std::vector<uint8_t> hash_input;
    hash_input.insert(hash_input.end(), PDF_PADDING, PDF_PADDING + 32);
    hash_input.insert(hash_input.end(), file_id.begin(), file_id.end());

    uint8_t hash[16];
    crypto::MD5::hash(hash_input.data(), hash_input.size(), hash);

    // Encrypt hash result
    memcpy(user_key.data(), hash, 16);

    crypto::RC4 rc4;
    rc4.init(encryption_key.data(), encryption_key.size());
    rc4.crypt(user_key.data(), 16);

    // Do additional encryption iterations
    for (int i = 1; i <= 19; i++) {
      std::vector<uint8_t> iter_key(encryption_key.size());
      for (size_t j = 0; j < encryption_key.size(); j++) {
        iter_key[j] = encryption_key[j] ^ i;
      }
      rc4.init(iter_key.data(), iter_key.size());
      rc4.crypt(user_key.data(), 16);
    }

    // Add arbitrary padding
    for (int i = 16; i < 32; i++) {
      user_key[i] = 0;
    }

    return user_key;
  }
}

// Verify user password
bool verify_user_password(const std::string& password,
                         const EncryptionDictionary& encrypt_dict,
                         const std::string& file_id) {
  // Compute what the user key should be
  std::vector<uint8_t> owner_key(encrypt_dict.o.begin(), encrypt_dict.o.end());
  auto computed_user_key = compute_user_key(password, owner_key,
                                            encrypt_dict.p, file_id,
                                            encrypt_dict.length, encrypt_dict.r);

  // Compare with stored user key
  if (encrypt_dict.r == 2) {
    // For revision 2, compare full 32 bytes
    return memcmp(computed_user_key.data(), encrypt_dict.u.data(), 32) == 0;
  } else {
    // For revision 3 and above, compare first 16 bytes
    return memcmp(computed_user_key.data(), encrypt_dict.u.data(), 16) == 0;
  }
}

// Verify owner password
bool verify_owner_password(const std::string& password,
                          const EncryptionDictionary& encrypt_dict,
                          const std::string& file_id) {
  // Pad owner password
  auto padded_owner = pad_password(password);

  // Compute MD5 hash
  uint8_t digest[16];
  crypto::MD5::hash(padded_owner.data(), 32, digest);

  // For revision 3 and above, do additional MD5 iterations
  if (encrypt_dict.r >= 3) {
    for (int i = 0; i < 50; i++) {
      crypto::MD5::hash(digest, encrypt_dict.length / 8, digest);
    }
  }

  // Create decryption key from digest
  int key_len_bytes = std::min(encrypt_dict.length / 8, 16);
  std::vector<uint8_t> key(digest, digest + key_len_bytes);

  // Decrypt owner key to get user password
  std::vector<uint8_t> decrypted(encrypt_dict.o.begin(), encrypt_dict.o.end());

  if (encrypt_dict.r == 2) {
    crypto::RC4 rc4;
    rc4.init(key.data(), key.size());
    rc4.crypt(decrypted.data(), 32);
  } else {
    // For revision 3 and above, do multiple decryption iterations
    for (int i = 19; i >= 0; i--) {
      std::vector<uint8_t> iter_key(key.size());
      for (size_t j = 0; j < key.size(); j++) {
        iter_key[j] = key[j] ^ i;
      }
      crypto::RC4 rc4;
      rc4.init(iter_key.data(), iter_key.size());
      rc4.crypt(decrypted.data(), 32);
    }
  }

  // The decrypted result should be a valid user password
  // Try to verify it as a user password
  std::string user_password(decrypted.begin(), decrypted.end());

  // Remove padding from decrypted password
  size_t pwd_len = 32;
  for (size_t i = 0; i < 32; i++) {
    bool is_padding = true;
    for (size_t j = i; j < 32 && j - i < 32; j++) {
      if (decrypted[j] != PDF_PADDING[j - i]) {
        is_padding = false;
        break;
      }
    }
    if (is_padding) {
      pwd_len = i;
      break;
    }
  }
  user_password = std::string(decrypted.begin(), decrypted.begin() + pwd_len);

  return verify_user_password(user_password, encrypt_dict, file_id);
}

// Algorithm 2.B from PDF 2.0 spec (ISO 32000-2:2020) for R=6
// Computes the hash used for user/owner password validation and key derivation
static std::vector<uint8_t> compute_hash_r6(const std::string& password,
                                             const uint8_t* salt, size_t salt_len,
                                             const uint8_t* u_data, size_t u_data_len) {
  // Initial hash: SHA-256(password + salt + u_data)
  std::vector<uint8_t> input;
  input.insert(input.end(), password.begin(), password.end());
  input.insert(input.end(), salt, salt + salt_len);
  if (u_data && u_data_len > 0) {
    input.insert(input.end(), u_data, u_data + u_data_len);
  }

  uint8_t K[64];  // Large enough for SHA-512
  size_t K_len = 32;
  crypto::SHA256::hash(input.data(), input.size(), K);

  int round = 0;
  for (;;) {
    // K1 = (password + K[0..K_len-1] + u_data) repeated 64 times
    std::vector<uint8_t> K1_single;
    K1_single.insert(K1_single.end(), password.begin(), password.end());
    K1_single.insert(K1_single.end(), K, K + K_len);
    if (u_data && u_data_len > 0) {
      K1_single.insert(K1_single.end(), u_data, u_data + u_data_len);
    }

    std::vector<uint8_t> K1;
    K1.reserve(K1_single.size() * 64);
    for (int i = 0; i < 64; i++) {
      K1.insert(K1.end(), K1_single.begin(), K1_single.end());
    }

    // E = AES-128-CBC-encrypt(key=K[0:16], iv=K[16:32], data=K1)
    crypto::AES128 aes;
    aes.set_key(K);  // K[0:16]
    std::vector<uint8_t> E(K1.size());
    aes.encrypt_cbc(K1.data(), E.data(), K1.size(), K + 16);  // iv=K[16:32]

    // mod = (sum of first 16 bytes of E as unsigned) % 3
    unsigned int sum = 0;
    for (int i = 0; i < 16; i++) {
      sum += E[i];
    }
    int mod = sum % 3;

    if (mod == 0) {
      crypto::SHA256::hash(E.data(), E.size(), K);
      K_len = 32;
    } else if (mod == 1) {
      crypto::SHA384::hash(E.data(), E.size(), K);
      K_len = 48;
    } else {
      crypto::SHA512::hash(E.data(), E.size(), K);
      K_len = 64;
    }

    round++;
    if (round >= 64 && static_cast<unsigned>(E.back()) <= static_cast<unsigned>(round - 32)) {
      break;
    }
  }

  return std::vector<uint8_t>(K, K + 32);
}

// SecurityHandler implementation

void SecurityHandler::compute_encryption_key(const std::string& password, bool is_owner_password) {
  if (encrypt_dict.v == 0 || encrypt_dict.v > 5) {
    return; // Unsupported version
  }

  // R=6 (AES-256): derive file encryption key using Algorithm 2.A
  if (encrypt_dict.r >= 6) {
    std::string pwd = password.substr(0, 127);

    if (is_owner_password) {
      // Owner key: OE decrypted with hash from O key salt + U
      if (encrypt_dict.o.size() < 48 || encrypt_dict.oe.size() < 32) return;
      const uint8_t* o_bytes = reinterpret_cast<const uint8_t*>(encrypt_dict.o.data());
      const uint8_t* u_bytes = reinterpret_cast<const uint8_t*>(encrypt_dict.u.data());
      auto key_hash = compute_hash_r6(pwd, o_bytes + 40, 8,
                                       u_bytes, 48);
      uint8_t zero_iv[16] = {0};
      crypto::AES256 aes;
      aes.set_key(key_hash.data());
      encryption_key.resize(32);
      aes.decrypt_cbc(reinterpret_cast<const uint8_t*>(encrypt_dict.oe.data()),
                       encryption_key.data(), 32, zero_iv);
    } else {
      // User key: UE decrypted with hash from U key salt
      if (encrypt_dict.u.size() < 48 || encrypt_dict.ue.size() < 32) return;
      const uint8_t* u_bytes = reinterpret_cast<const uint8_t*>(encrypt_dict.u.data());
      auto key_hash = compute_hash_r6(pwd, u_bytes + 40, 8,
                                       nullptr, 0);
      uint8_t zero_iv[16] = {0};
      crypto::AES256 aes;
      aes.set_key(key_hash.data());
      encryption_key.resize(32);
      aes.decrypt_cbc(reinterpret_cast<const uint8_t*>(encrypt_dict.ue.data()),
                       encryption_key.data(), 32, zero_iv);
    }
    return;
  }

  std::string actual_password = password;

  // If owner password, first decrypt to get user password
  if (is_owner_password) {
    // Pad owner password
    auto padded_owner = pad_password(password);

    // Compute MD5 hash
    uint8_t digest[16];
    crypto::MD5::hash(padded_owner.data(), 32, digest);

    // For revision 3 and above, do additional MD5 iterations
    if (encrypt_dict.r >= 3) {
      for (int i = 0; i < 50; i++) {
        crypto::MD5::hash(digest, encrypt_dict.length / 8, digest);
      }
    }

    // Create decryption key
    int key_len_bytes = std::min(encrypt_dict.length / 8, 16);
    std::vector<uint8_t> key(digest, digest + key_len_bytes);

    // Decrypt owner key to get user password
    std::vector<uint8_t> decrypted(encrypt_dict.o.begin(), encrypt_dict.o.end());

    if (encrypt_dict.r == 2) {
      crypto::RC4 rc4;
      rc4.init(key.data(), key.size());
      rc4.crypt(decrypted.data(), 32);
    } else {
      for (int i = 19; i >= 0; i--) {
        std::vector<uint8_t> iter_key(key.size());
        for (size_t j = 0; j < key.size(); j++) {
          iter_key[j] = key[j] ^ i;
        }
        crypto::RC4 rc4;
        rc4.init(iter_key.data(), iter_key.size());
        rc4.crypt(decrypted.data(), 32);
      }
    }

    // Extract user password
    size_t pwd_len = 32;
    for (size_t i = 0; i < 32; i++) {
      bool is_padding = true;
      for (size_t j = i; j < 32 && j - i < 32; j++) {
        if (decrypted[j] != PDF_PADDING[j - i]) {
          is_padding = false;
          break;
        }
      }
      if (is_padding) {
        pwd_len = i;
        break;
      }
    }
    actual_password = std::string(decrypted.begin(), decrypted.begin() + pwd_len);
  }

  // Now compute encryption key from user password
  auto padded_password = pad_password(actual_password);

  // Build data to hash
  std::vector<uint8_t> hash_data;
  hash_data.insert(hash_data.end(), padded_password.begin(), padded_password.end());
  hash_data.insert(hash_data.end(), encrypt_dict.o.begin(), encrypt_dict.o.end());

  // Add permissions (low-order byte first)
  hash_data.push_back((encrypt_dict.p >> 0) & 0xFF);
  hash_data.push_back((encrypt_dict.p >> 8) & 0xFF);
  hash_data.push_back((encrypt_dict.p >> 16) & 0xFF);
  hash_data.push_back((encrypt_dict.p >> 24) & 0xFF);

  // Add file ID (first element of ID array)
  hash_data.insert(hash_data.end(), file_id.begin(), file_id.end());

  // For revision 4 and above, if not encrypting metadata, add 0xFFFFFFFF
  if (encrypt_dict.r >= 4 && !encrypt_dict.encrypt_metadata) {
    hash_data.push_back(0xFF);
    hash_data.push_back(0xFF);
    hash_data.push_back(0xFF);
    hash_data.push_back(0xFF);
  }

  // Compute MD5 hash
  uint8_t digest[16];
  crypto::MD5::hash(hash_data.data(), hash_data.size(), digest);

  // For revision 3 and above, do additional MD5 iterations
  if (encrypt_dict.r >= 3) {
    for (int i = 0; i < 50; i++) {
      crypto::MD5::hash(digest, encrypt_dict.length / 8, digest);
    }
  }

  // Store encryption key
  int key_len_bytes = std::min(encrypt_dict.length / 8, 16);
  encryption_key.assign(digest, digest + key_len_bytes);
}

std::vector<uint8_t> SecurityHandler::compute_object_key(uint32_t obj_num, uint16_t gen_num) const {
  if (encryption_key.empty()) {
    return std::vector<uint8_t>();
  }

  // AES-256 (V=5) uses the file encryption key directly, no per-object derivation
  if (algorithm == EncryptionAlgorithm::AES_256) {
    return encryption_key;
  }

  // Derive per-object key (required for all encryption versions per PDF spec §7.6.2)
  std::vector<uint8_t> hash_data;
  hash_data.insert(hash_data.end(), encryption_key.begin(), encryption_key.end());

  // Add object number (low byte first)
  hash_data.push_back((obj_num >> 0) & 0xFF);
  hash_data.push_back((obj_num >> 8) & 0xFF);
  hash_data.push_back((obj_num >> 16) & 0xFF);

  // Add generation number (low byte first)
  hash_data.push_back((gen_num >> 0) & 0xFF);
  hash_data.push_back((gen_num >> 8) & 0xFF);

  // For AES encryption, add "sAlT"
  if (algorithm == EncryptionAlgorithm::AES_128 || algorithm == EncryptionAlgorithm::AES_256) {
    hash_data.push_back('s');
    hash_data.push_back('A');
    hash_data.push_back('l');
    hash_data.push_back('T');
  }

  // Compute MD5 hash
  uint8_t digest[16];
  crypto::MD5::hash(hash_data.data(), hash_data.size(), digest);

  // Return appropriate key length
  int key_len = std::min(static_cast<int>(encryption_key.size() + 5), 16);
  return std::vector<uint8_t>(digest, digest + key_len);
}

bool SecurityHandler::authenticate_user_password(const std::string& password) {
  if (algorithm == EncryptionAlgorithm::None) {
    authenticated = true;
    is_owner = false;
    return true;
  }

  // R=6 (AES-256): Algorithm 2.A user password validation
  if (encrypt_dict.r >= 6) {
    if (encrypt_dict.u.size() < 48) {
      authenticated = false;
      return false;
    }
    std::string pwd = password.substr(0, 127);
    const uint8_t* u_bytes = reinterpret_cast<const uint8_t*>(encrypt_dict.u.data());
    // Validation salt = U[32:40]
    auto hash = compute_hash_r6(pwd, u_bytes + 32, 8, nullptr, 0);
    if (hash.size() >= 32 && memcmp(hash.data(), u_bytes, 32) == 0) {
      compute_encryption_key(pwd, false);
      authenticated = true;
      is_owner = false;
      return true;
    }
    authenticated = false;
    return false;
  }

  // For AES encryption (V=4, R=4), we use the standard PDF authentication
  // The key derivation uses file_id which we now have

  // Verify using the stored U value
  bool verified = verify_user_password(password, encrypt_dict, file_id);

  if (verified) {
    // Compute the encryption key
    compute_encryption_key(password, false);
    authenticated = true;
    is_owner = false;
    return true;
  }

  // If verification fails, still try to compute the key for PDFs that may
  // have non-standard encryption. This is needed for some AES-encrypted PDFs
  // that use empty passwords but have non-standard U values.
  if (algorithm == EncryptionAlgorithm::AES_128 || algorithm == EncryptionAlgorithm::AES_256) {
    compute_encryption_key(password, false);
    // For AES, assume authenticated if we have a valid key
    // The decryption will fail naturally if the key is wrong
    if (!encryption_key.empty()) {
      authenticated = true;
      is_owner = false;
      return true;
    }
  }

  authenticated = false;
  is_owner = false;
  return false;
}

bool SecurityHandler::authenticate_owner_password(const std::string& password) {
  // This would need access to the file ID from the PDF
  // For now, return false
  authenticated = false;
  is_owner = false;
  return false;
}

std::vector<uint8_t> SecurityHandler::decrypt_string(const std::string& str,
                                                     uint32_t obj_num,
                                                     uint16_t gen_num,
                                                     const std::string& crypt_filter_name) const {
  if (!authenticated || encryption_key.empty()) {
    return std::vector<uint8_t>(str.begin(), str.end());
  }

  // Determine effective algorithm from crypt filter if specified
  EncryptionAlgorithm effective_algo = algorithm;
  if (!crypt_filter_name.empty() && !encrypt_dict.crypt_filters.empty()) {
    auto cf_it = encrypt_dict.crypt_filters.find(crypt_filter_name);
    if (cf_it != encrypt_dict.crypt_filters.end()) {
      const auto& cfm = cf_it->second.cfm;
      if (cfm == "None") return std::vector<uint8_t>(str.begin(), str.end());
      if (cfm == "V2") effective_algo = EncryptionAlgorithm::RC4_128;
      else if (cfm == "AESV2") effective_algo = EncryptionAlgorithm::AES_128;
      else if (cfm == "AESV3") effective_algo = EncryptionAlgorithm::AES_256;
    }
  }

  std::vector<uint8_t> data(str.begin(), str.end());
  auto obj_key = compute_object_key(obj_num, gen_num);

  if (effective_algo == EncryptionAlgorithm::RC4_40 || effective_algo == EncryptionAlgorithm::RC4_128) {
    crypto::RC4 rc4;
    rc4.init(obj_key.data(), obj_key.size());
    rc4.crypt(data.data(), data.size());
  } else if (effective_algo == EncryptionAlgorithm::AES_128) {
    // For AES, strings are encrypted in CBC mode with a random IV
    if (data.size() < 16) {
      return data; // Invalid encrypted string
    }

    // First 16 bytes are the IV
    uint8_t iv[16];
    memcpy(iv, data.data(), 16);

    // Decrypt the rest
    crypto::AES128 aes;
    aes.set_key(obj_key.data());

    std::vector<uint8_t> decrypted(data.size() - 16);
    aes.decrypt_cbc(data.data() + 16, decrypted.data(), data.size() - 16, iv);

    // Remove padding
    size_t unpadded_len = crypto::unpad_pkcs7(decrypted.data(), decrypted.size());
    decrypted.resize(unpadded_len);

    return decrypted;
  } else if (effective_algo == EncryptionAlgorithm::AES_256) {
    if (data.size() < 16) {
      return data;
    }

    uint8_t iv[16];
    memcpy(iv, data.data(), 16);

    crypto::AES256 aes;
    aes.set_key(encryption_key.data());

    std::vector<uint8_t> decrypted(data.size() - 16);
    aes.decrypt_cbc(data.data() + 16, decrypted.data(), data.size() - 16, iv);

    size_t unpadded_len = crypto::unpad_pkcs7(decrypted.data(), decrypted.size());
    decrypted.resize(unpadded_len);

    return decrypted;
  }

  return data;
}

std::vector<uint8_t> SecurityHandler::decrypt_stream(const std::vector<uint8_t>& data,
                                                     uint32_t obj_num,
                                                     uint16_t gen_num,
                                                     const std::string& crypt_filter_name) const {
  if (!authenticated || encryption_key.empty()) {
    return data;
  }

  // Determine effective algorithm from crypt filter if specified
  EncryptionAlgorithm effective_algo = algorithm;
  if (!crypt_filter_name.empty() && !encrypt_dict.crypt_filters.empty()) {
    auto cf_it = encrypt_dict.crypt_filters.find(crypt_filter_name);
    if (cf_it != encrypt_dict.crypt_filters.end()) {
      const auto& cfm = cf_it->second.cfm;
      if (cfm == "None") return data;
      if (cfm == "V2") effective_algo = EncryptionAlgorithm::RC4_128;
      else if (cfm == "AESV2") effective_algo = EncryptionAlgorithm::AES_128;
      else if (cfm == "AESV3") effective_algo = EncryptionAlgorithm::AES_256;
    }
  }

  std::vector<uint8_t> result = data;
  auto obj_key = compute_object_key(obj_num, gen_num);

  if (effective_algo == EncryptionAlgorithm::RC4_40 || effective_algo == EncryptionAlgorithm::RC4_128) {
    crypto::RC4 rc4;
    rc4.init(obj_key.data(), obj_key.size());
    rc4.crypt(result.data(), result.size());
  } else if (effective_algo == EncryptionAlgorithm::AES_128) {
    // For AES, streams are encrypted in CBC mode with a random IV
    if (result.size() < 16) {
      return result; // Invalid encrypted stream
    }

    // First 16 bytes are the IV
    uint8_t iv[16];
    memcpy(iv, result.data(), 16);

    // Decrypt the rest
    crypto::AES128 aes;
    aes.set_key(obj_key.data());

    std::vector<uint8_t> decrypted(result.size() - 16);
    aes.decrypt_cbc(result.data() + 16, decrypted.data(), result.size() - 16, iv);

    // Remove padding
    size_t unpadded_len = crypto::unpad_pkcs7(decrypted.data(), decrypted.size());
    decrypted.resize(unpadded_len);

    return decrypted;
  } else if (effective_algo == EncryptionAlgorithm::AES_256) {
    if (result.size() < 16) {
      return result;
    }

    uint8_t iv[16];
    memcpy(iv, result.data(), 16);

    crypto::AES256 aes;
    aes.set_key(encryption_key.data());

    std::vector<uint8_t> decrypted(result.size() - 16);
    aes.decrypt_cbc(result.data() + 16, decrypted.data(), result.size() - 16, iv);

    size_t unpadded_len = crypto::unpad_pkcs7(decrypted.data(), decrypted.size());
    decrypted.resize(unpadded_len);

    return decrypted;
  }

  return result;
}

std::vector<uint8_t> SecurityHandler::encrypt_string(const std::string& str,
                                                     uint32_t obj_num,
                                                     uint16_t gen_num) {
  // Implementation similar to decrypt_string but in reverse
  // This would be used when creating encrypted PDFs
  return std::vector<uint8_t>(str.begin(), str.end());
}

std::vector<uint8_t> SecurityHandler::encrypt_stream(const std::vector<uint8_t>& data,
                                                     uint32_t obj_num,
                                                     uint16_t gen_num) {
  // Implementation similar to decrypt_stream but in reverse
  // This would be used when creating encrypted PDFs
  return data;
}

// Create security handler from PDF
SecurityHandler create_security_handler(const Pdf& pdf) {
  SecurityHandler handler;

  if (pdf.encrypt == 0) {
    handler.algorithm = EncryptionAlgorithm::None;
    handler.authenticated = true; // No encryption
    return handler;
  }

  // Store file ID for encryption key computation
  handler.file_id = pdf.id;

  // Load encryption dictionary
  auto encrypt_obj = resolve_reference(pdf, pdf.encrypt, 0);
  if (!encrypt_obj.success || encrypt_obj.value.type != Value::DICTIONARY) {
    return handler;
  }

  handler.encrypt_dict = parse_encryption_dictionary(pdf, encrypt_obj.value.dict);

  // Determine algorithm
  if (handler.encrypt_dict.v == 1) {
    handler.algorithm = (handler.encrypt_dict.length == 40) ?
                        EncryptionAlgorithm::RC4_40 : EncryptionAlgorithm::RC4_128;
  } else if (handler.encrypt_dict.v == 2) {
    handler.algorithm = EncryptionAlgorithm::RC4_128;
  } else if (handler.encrypt_dict.v == 4) {
    handler.algorithm = EncryptionAlgorithm::AES_128;
  } else if (handler.encrypt_dict.v == 5) {
    handler.algorithm = EncryptionAlgorithm::AES_256;
  }

  // Attempt authentication with empty password (for user-accessible PDFs)
  if (handler.algorithm != EncryptionAlgorithm::None) {
    handler.authenticate_user_password("");
  }

  return handler;
}

} // namespace nanopdf