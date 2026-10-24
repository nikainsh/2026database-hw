/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/// 定长小端字节序的序列化缓冲，用于 catalog / 文件头等元数据
class BinaryWriter
{
public:
  void put_i32(int32_t v)
  {
    uint8_t b[4];
    encode(v, b);
    buf_.insert(buf_.end(), b, b + 4);
  }

  void put_i64(int64_t v)
  {
    uint8_t b[8];
    encode(v, b);
    buf_.insert(buf_.end(), b, b + 8);
  }

  void put_bool(bool v) { buf_.push_back(v ? 1 : 0); }

  void put_string(const std::string &s)
  {
    put_i32(static_cast<int32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
  }

  void put_raw(const void *data, size_t len)
  {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    buf_.insert(buf_.end(), p, p + len);
  }

  const std::vector<uint8_t> &data() const { return buf_; }

private:
  template <typename T>
  void encode(T v, uint8_t *out)
  {
    uint64_t u = static_cast<uint64_t>(v);
    for (size_t i = 0; i < sizeof(T); ++i) {
      out[i] = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
    }
  }

  std::vector<uint8_t> buf_;
};

/// 读取时不会抛异常，越界只置 ok_=false，调用方在结束时检查一次即可
class BinaryReader
{
public:
  BinaryReader(const uint8_t *data, size_t len) : data_(data), len_(len) {}

  bool ok() const { return ok_; }

  int32_t get_i32()
  {
    int32_t v = 0;
    if (remain() >= 4) {
      uint32_t u = 0;
      for (int i = 3; i >= 0; --i) {
        u = (u << 8) | data_[pos_ + i];
      }
      memcpy(&v, &u, 4);
      pos_ += 4;
    } else {
      ok_ = false;
    }
    return v;
  }

  int64_t get_i64()
  {
    int64_t v = 0;
    if (remain() >= 8) {
      uint64_t u = 0;
      for (int i = 7; i >= 0; --i) {
        u = (u << 8) | data_[pos_ + i];
      }
      memcpy(&v, &u, 8);
      pos_ += 8;
    } else {
      ok_ = false;
    }
    return v;
  }

  bool get_bool()
  {
    if (remain() < 1) {
      ok_ = false;
      return false;
    }
    return data_[pos_++] != 0;
  }

  std::string get_string()
  {
    const int32_t len = get_i32();
    if (len < 0 || static_cast<size_t>(len) > remain()) {
      ok_ = false;
      return "";
    }
    std::string s(reinterpret_cast<const char *>(data_ + pos_), static_cast<size_t>(len));
    pos_ += static_cast<size_t>(len);
    return s;
  }

  void get_raw(void *out, size_t len)
  {
    if (len > remain()) {
      ok_ = false;
      return;
    }
    memcpy(out, data_ + pos_, len);
    pos_ += len;
  }

  size_t remain() const { return pos_ <= len_ ? len_ - pos_ : 0; }

private:
  const uint8_t *data_ = nullptr;
  size_t         len_  = 0;
  size_t         pos_  = 0;
  bool           ok_   = true;
};
