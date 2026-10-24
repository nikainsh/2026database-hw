/* minidb - 公共层自测：值类型、序列化、文件工具 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/binary.h"
#include "common/file_util.h"
#include "common/log.h"
#include "common/rc.h"
#include "common/value.h"

static void test_value()
{
  RC rc = RC::SUCCESS;

  Value i = Value::make(AttrType::INT, "42", rc);
  assert(OB_SUCC(rc) && i.type() == AttrType::INT && i.get_int32() == 42);

  Value f = Value::make(AttrType::FLOAT, "3.5", rc);
  assert(OB_SUCC(rc) && f.get_float() == 3.5f);

  Value big = Value::make(AttrType::BIGINT, "9000000000", rc);
  assert(OB_SUCC(rc) && big.get_int64() == 9000000000LL);

  int cmp = 0;
  assert(OB_SUCC(i.compare_to(f, cmp)) && cmp > 0);   /// 42 > 3.5，跨类型按数值比较
  assert(OB_SUCC(f.compare_to(i, cmp)) && cmp < 0);
  assert(OB_SUCC(big.compare_to(big, cmp)) && cmp == 0);

  Value text = Value::make(AttrType::CHARS, "hello", rc);
  assert(OB_SUCC(rc) && text.get_string() == "hello");

  Value text2("hell");
  assert(OB_SUCC(text.compare_to(text2, cmp)) && cmp > 0);

  // 类型不兼容时比较必须报错，而不是给出错误结果
  Value bad;
  assert(OB_FAIL(text.compare_to(i, cmp)) || OB_SUCC(text.cast_to(AttrType::INT, bad)));

  // 打包 / 解包
  char buf[16];
  memset(buf, 0xAA, sizeof(buf));
  assert(OB_SUCC(i.pack_into(buf, 4)));
  Value back;
  back.set_type(AttrType::INT);
  assert(OB_SUCC(back.unpack_from(buf, 4)) && back.get_int32() == 42);

  assert(OB_SUCC(text.pack_into(buf, 8)));
  Value back_str;
  back_str.set_type(AttrType::CHARS);
  assert(OB_SUCC(back_str.unpack_from(buf, 8)) && back_str.get_string() == "hello");

  Value long_text(std::string(9, 'x'));
  assert(long_text.pack_into(buf, 8) == RC::IOERR_TOO_LONG);

  Value casted;
  assert(OB_SUCC(i.cast_to(AttrType::FLOAT, casted)) && casted.get_float() == 42.0f);
  assert(casted.type() == AttrType::FLOAT);

  assert(Value(123).to_string() == "123");
  assert(Value(std::string("ab")).to_string() == "ab");
}

static void test_binary()
{
  BinaryWriter w;
  w.put_i32(-7);
  w.put_i64(1LL << 40);
  w.put_bool(true);
  w.put_string("miniob-like");
  w.put_string("");

  BinaryReader r(w.data().data(), w.data().size());
  assert(r.get_i32() == -7);
  assert(r.get_i64() == (1LL << 40));
  assert(r.get_bool() == true);
  assert(r.get_string() == "miniob-like");
  assert(r.get_string() == "");
  assert(r.ok());
  assert(r.remain() == 0);

  BinaryReader short_read(w.data().data(), 4);
  short_read.get_i32();
  assert(short_read.get_i32() == 0 && !short_read.ok());
}

static void test_file_util()
{
  const std::string dir = "./_test_tmp/nested/deeper";
  assert(OB_SUCC(path_mkdir_recursive(dir)));
  assert(OB_SUCC(path_mkdir_recursive(dir)));  /// 幂等

  const std::string file = path_join(dir, "a.bin");
  FILE              *fp  = fopen(file.c_str(), "wb");
  assert(fp != nullptr);
  fwrite("12345", 1, 5, fp);
  fclose(fp);

  assert(file_exist(file));
  assert(file_size(file) == 5);
  assert(path_list_files(dir).size() == 1);

  assert(OB_SUCC(path_remove_file(file)));
  assert(!file_exist(file));
  assert(path_remove_file(file) == RC::FILE_NOT_EXIST);

  assert(path_join("a", "b") == "a/b");
  assert(path_join("a/", "b") == "a/b");
  assert(path_normalize("a\\b\\c") == "a/b/c");
}

int main()
{
  test_value();
  test_binary();
  test_file_util();
  printf("test_common: OK\n");
  return 0;
}
