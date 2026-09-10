#include "core/sha256.h"

#include <cstring>

namespace cv {
namespace {

constexpr unsigned int kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline unsigned int rotr(unsigned int x, int n) {
  return (x >> n) | (x << (32 - n));
}

inline unsigned int be32(const unsigned char* p) {
  return (static_cast<unsigned int>(p[0]) << 24) |
         (static_cast<unsigned int>(p[1]) << 16) |
         (static_cast<unsigned int>(p[2]) << 8) |
         (static_cast<unsigned int>(p[3]));
}

}  // namespace

Sha256::Sha256() { reset(); }

void Sha256::reset() {
  state_[0] = 0x6a09e667u;
  state_[1] = 0xbb67ae85u;
  state_[2] = 0x3c6ef372u;
  state_[3] = 0xa54ff53au;
  state_[4] = 0x510e527fu;
  state_[5] = 0x9b05688cu;
  state_[6] = 0x1f83d9abu;
  state_[7] = 0x5be0cd19u;
  bitLen_ = 0;
  bufLen_ = 0;
  done_ = false;
  cached_.clear();
  std::memset(buf_, 0, sizeof(buf_));
}

void Sha256::compress(const unsigned char* block) {
  unsigned int w[64];
  for (int i = 0; i < 16; ++i) w[i] = be32(block + i * 4);
  for (int i = 16; i < 64; ++i) {
    unsigned int s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    unsigned int s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  unsigned int a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  unsigned int e = state_[4], f = state_[5], g = state_[6], h = state_[7];

  for (int i = 0; i < 64; ++i) {
    unsigned int t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
                      ((e & f) ^ ((~e) & g)) + kK[i] + w[i];
    unsigned int t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t len) {
  if (done_ || len == 0) return;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  bitLen_ += static_cast<unsigned long long>(len) * 8ULL;

  while (len > 0) {
    std::size_t n = 64 - bufLen_;
    if (n > len) n = len;
    std::memcpy(buf_ + bufLen_, p, n);
    bufLen_ += n;
    p += n;
    len -= n;
    if (bufLen_ == 64) {
      compress(buf_);
      bufLen_ = 0;
    }
  }
}

void Sha256::update(const std::string& data) { update(data.data(), data.size()); }

void Sha256::finalize() {
  if (done_) return;
  unsigned long long totalBits = bitLen_;

  std::size_t padLen = (bufLen_ < 56) ? (56 - bufLen_) : (120 - bufLen_);
  unsigned char pad[128];
  std::memset(pad, 0, sizeof(pad));
  pad[0] = 0x80;
  update(pad, padLen);

  unsigned char lenBytes[8];
  for (int i = 0; i < 8; ++i) {
    lenBytes[i] = static_cast<unsigned char>((totalBits >> (56 - i * 8)) & 0xff);
  }
  update(lenBytes, 8);

  static const char* kHex = "0123456789abcdef";
  cached_.clear();
  cached_.reserve(64);
  for (int i = 0; i < 8; ++i) {
    unsigned int v = state_[i];
    for (int j = 7; j >= 0; --j) {
      cached_.push_back(kHex[(v >> (j * 4)) & 0xf]);
    }
  }
  done_ = true;
}

std::string Sha256::hex() {
  finalize();
  return cached_;
}

std::string Sha256::of(const void* data, std::size_t len) {
  Sha256 s;
  s.update(data, len);
  return s.hex();
}

std::string Sha256::of(const std::string& data) {
  return of(data.data(), data.size());
}

}  // namespace cv
