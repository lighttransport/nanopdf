// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "ecc.hh"

#include <string>
#include <vector>

namespace nanopdf {
namespace crypto {

namespace {

// Parse a hex string into a BigInt.
BigInt big_hex(const char* h) {
  std::vector<uint8_t> bytes;
  auto nib = [](char c) {
    return c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
  };
  std::string s;
  for (size_t i = 0; h[i]; ++i)
    if (h[i] != ' ') s += h[i];
  if (s.size() & 1) s = "0" + s;
  for (size_t i = 0; i < s.size(); i += 2)
    bytes.push_back((uint8_t)(nib(s[i]) * 16 + nib(s[i + 1])));
  return BigInt::from_bytes(bytes.data(), bytes.size());
}

// Modular helpers over a prime modulus m.
BigInt mod_m(const BigInt& a, const BigInt& m) { return BigInt::mod(a, m); }
BigInt mmul(const BigInt& a, const BigInt& b, const BigInt& m) {
  return BigInt::mod(BigInt::mul(a, b), m);
}
BigInt madd(const BigInt& a, const BigInt& b, const BigInt& m) {
  return BigInt::mod(BigInt::add(a, b), m);
}
// (a - b) mod m, for any a,b >= 0.
BigInt msub(const BigInt& a, const BigInt& b, const BigInt& m) {
  BigInt am = BigInt::mod(a, m), bm = BigInt::mod(b, m);
  if (BigInt::cmp(am, bm) >= 0) return BigInt::sub(am, bm);
  return BigInt::sub(BigInt::add(am, m), bm);
}
// Modular inverse via Fermat: a^(m-2) mod m (m prime).
BigInt minv(const BigInt& a, const BigInt& m) {
  BigInt e = BigInt::sub(m, BigInt(2));
  return BigInt::modexp(BigInt::mod(a, m), e, m);
}

// A point in Jacobian projective coordinates (X:Y:Z) -> affine (X/Z^2, Y/Z^3).
struct Jac {
  BigInt X, Y, Z;
  bool inf = false;
};

Jac jac_double(const Jac& P, const EcCurve& c) {
  if (P.inf || P.Y.is_zero()) return Jac{BigInt(), BigInt(), BigInt(), true};
  const BigInt& p = c.p;
  BigInt YY = mmul(P.Y, P.Y, p);
  BigInt S = mmul(BigInt(4), mmul(P.X, YY, p), p);          // 4*X*Y^2
  BigInt ZZ = mmul(P.Z, P.Z, p);
  BigInt Z4 = mmul(ZZ, ZZ, p);
  BigInt M = madd(mmul(BigInt(3), mmul(P.X, P.X, p), p),    // 3X^2 + a*Z^4
                  mmul(c.a, Z4, p), p);
  BigInt X3 = msub(mmul(M, M, p), mmul(BigInt(2), S, p), p);  // M^2 - 2S
  BigInt YYYY = mmul(YY, YY, p);
  BigInt Y3 = msub(mmul(M, msub(S, X3, p), p),               // M(S-X3) - 8Y^4
                   mmul(BigInt(8), YYYY, p), p);
  BigInt Z3 = mmul(BigInt(2), mmul(P.Y, P.Z, p), p);         // 2YZ
  return Jac{X3, Y3, Z3, false};
}

Jac jac_add(const Jac& P, const Jac& Q, const EcCurve& c) {
  if (P.inf) return Q;
  if (Q.inf) return P;
  const BigInt& p = c.p;
  BigInt Z1Z1 = mmul(P.Z, P.Z, p);
  BigInt Z2Z2 = mmul(Q.Z, Q.Z, p);
  BigInt U1 = mmul(P.X, Z2Z2, p);                 // X1*Z2^2
  BigInt U2 = mmul(Q.X, Z1Z1, p);                 // X2*Z1^2
  BigInt S1 = mmul(P.Y, mmul(Z2Z2, Q.Z, p), p);   // Y1*Z2^3
  BigInt S2 = mmul(Q.Y, mmul(Z1Z1, P.Z, p), p);   // Y2*Z1^3
  if (BigInt::cmp(U1, U2) == 0) {
    if (BigInt::cmp(S1, S2) != 0)
      return Jac{BigInt(), BigInt(), BigInt(), true};  // P + (-P) = O
    return jac_double(P, c);
  }
  BigInt H = msub(U2, U1, p);
  BigInt R = msub(S2, S1, p);
  BigInt HH = mmul(H, H, p);
  BigInt HHH = mmul(HH, H, p);
  BigInt U1HH = mmul(U1, HH, p);
  BigInt X3 = msub(msub(mmul(R, R, p), HHH, p),    // R^2 - H^3 - 2*U1*H^2
                   mmul(BigInt(2), U1HH, p), p);
  BigInt Y3 = msub(mmul(R, msub(U1HH, X3, p), p),  // R*(U1*H^2 - X3) - S1*H^3
                   mmul(S1, HHH, p), p);
  BigInt Z3 = mmul(H, mmul(P.Z, Q.Z, p), p);       // H*Z1*Z2
  return Jac{X3, Y3, Z3, false};
}

// k*P via left-to-right double-and-add over the big-endian bits of k.
Jac jac_mul(const BigInt& k, const Jac& P, const EcCurve& c) {
  Jac R{BigInt(), BigInt(), BigInt(), true};
  std::vector<uint8_t> kb = k.to_bytes();
  bool started = false;
  for (size_t i = 0; i < kb.size(); ++i) {
    for (int b = 7; b >= 0; --b) {
      if (started) R = jac_double(R, c);
      if ((kb[i] >> b) & 1) {
        R = started ? jac_add(R, P, c) : P;
        started = true;
      }
    }
  }
  return R;
}

// Affine x-coordinate of a Jacobian point (returns false if point at infinity).
bool jac_affine_x(const Jac& P, const EcCurve& c, BigInt* x) {
  if (P.inf || P.Z.is_zero()) return false;
  BigInt zinv = minv(P.Z, c.p);
  BigInt zinv2 = mmul(zinv, zinv, c.p);
  *x = mmul(P.X, zinv2, c.p);
  return true;
}

EcCurve make_p256() {
  EcCurve c;
  c.p = big_hex("FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF");
  c.a = big_hex("FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFC");
  c.b = big_hex("5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B");
  c.gx = big_hex("6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296");
  c.gy = big_hex("4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5");
  c.n = big_hex("FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551");
  c.field_bytes = 32;
  c.valid = true;
  return c;
}

EcCurve make_p384() {
  EcCurve c;
  c.p = big_hex("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE"
                "FFFFFFFF0000000000000000FFFFFFFF");
  c.a = big_hex("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE"
                "FFFFFFFF0000000000000000FFFFFFFC");
  c.b = big_hex("B3312FA7E23EE7E4988E056BE3F82D19181D9C6EFE8141120314088F5013875A"
                "C656398D8A2ED19D2A85C8EDD3EC2AEF");
  c.gx = big_hex("AA87CA22BE8B05378EB1C71EF320AD746E1D3B628BA79B9859F741E082542A38"
                 "5502F25DBF55296C3A545E3872760AB7");
  c.gy = big_hex("3617DE4A96262C6F5D9E98BF9292DC29F8F41DBD289A147CE9DA3113B5F0B8C0"
                 "0A60B1CE1D7E819D7A431D7C90EA0E5F");
  c.n = big_hex("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF"
                "581A0DB248B0A77AECEC196ACCC52973");
  c.field_bytes = 48;
  c.valid = true;
  return c;
}

}  // namespace

const EcCurve& curve_p256() {
  static const EcCurve c = make_p256();
  return c;
}
const EcCurve& curve_p384() {
  static const EcCurve c = make_p384();
  return c;
}

bool ecdsa_verify(const EcCurve& curve, const uint8_t* pub, size_t pub_len,
                  const uint8_t* hash, size_t hash_len, const uint8_t* r_be,
                  size_t r_len, const uint8_t* s_be, size_t s_len) {
  if (!curve.valid) return false;
  size_t fb = curve.field_bytes;

  // Parse the public point (uncompressed SEC1, optional 0x04 prefix).
  if (pub_len == 2 * fb + 1) {
    if (pub[0] != 0x04) return false;  // only uncompressed supported
    ++pub;
    pub_len -= 1;
  }
  if (pub_len != 2 * fb) return false;
  BigInt Qx = BigInt::from_bytes(pub, fb);
  BigInt Qy = BigInt::from_bytes(pub + fb, fb);

  // Validate the public key: coordinates in [0,p), not the identity, and on the
  // curve (y^2 == x^3 + a*x + b mod p). Rejects invalid-curve points.
  if (BigInt::cmp(Qx, curve.p) >= 0 || BigInt::cmp(Qy, curve.p) >= 0)
    return false;
  if (Qx.is_zero() && Qy.is_zero()) return false;
  {
    BigInt lhs = mmul(Qy, Qy, curve.p);
    BigInt rhs = mmul(mmul(Qx, Qx, curve.p), Qx, curve.p);
    rhs = madd(rhs, mmul(curve.a, Qx, curve.p), curve.p);
    rhs = madd(rhs, curve.b, curve.p);
    if (BigInt::cmp(lhs, rhs) != 0) return false;
  }

  BigInt r = BigInt::from_bytes(r_be, r_len);
  BigInt s = BigInt::from_bytes(s_be, s_len);
  // 1 <= r,s < n
  if (r.is_zero() || s.is_zero()) return false;
  if (BigInt::cmp(r, curve.n) >= 0 || BigInt::cmp(s, curve.n) >= 0) return false;

  // z = the leftmost Ln bits of the hash (Ln = bit length of n).
  BigInt z = BigInt::from_bytes(hash, hash_len);
  size_t nbits = curve.n.bit_length();
  if (hash_len * 8 > nbits) {
    // Right-shift z by (hash_len*8 - nbits) bits via division by 2^shift.
    size_t shift = hash_len * 8 - nbits;
    std::vector<uint8_t> div(shift / 8 + 1, 0);
    div[0] = (uint8_t)(1u << (shift % 8));  // 2^shift, big-endian
    BigInt divisor = BigInt::from_bytes(div.data(), div.size());
    BigInt q, rem;
    BigInt::divmod(z, divisor, &q, &rem);
    z = q;
  }

  BigInt w = minv(s, curve.n);
  BigInt u1 = mmul(mod_m(z, curve.n), w, curve.n);
  BigInt u2 = mmul(mod_m(r, curve.n), w, curve.n);

  Jac G{curve.gx, curve.gy, BigInt(1), false};
  Jac Q{Qx, Qy, BigInt(1), false};
  Jac R = jac_add(jac_mul(u1, G, curve), jac_mul(u2, Q, curve), curve);

  BigInt x;
  if (!jac_affine_x(R, curve, &x)) return false;
  BigInt v = mod_m(x, curve.n);
  return BigInt::cmp(v, r) == 0;
}

}  // namespace crypto
}  // namespace nanopdf
