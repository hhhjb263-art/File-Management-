// 核心模块自检：SHA-256 / JSON / 内容寻址存储
// 构建：cmake --build build --target cv_tests && ./build/cv_tests

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "core/json.h"
#include "core/sha256.h"
#include "store/content_store.h"

namespace fs = std::filesystem;

namespace {

int g_failed = 0;

void check(bool ok, const char* expr, int line) {
  if (!ok) {
    std::printf("[FAIL] line %d: %s\n", line, expr);
    ++g_failed;
  }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void testSha256() {
  CHECK(cv::Sha256::of("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(cv::Sha256::of("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(cv::Sha256::of(std::string(1000000, 'a')) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  cv::Sha256 s;
  s.update("hello ");
  s.update("world");
  CHECK(s.hex() == cv::Sha256::of("hello world"));
}

void testJson() {
  std::string text = R"({"name":"报告.docx","size":2048,"tags":["a","b"],"ok":true,"x":null,"n":1.5})";
  cv::json::Value v;
  std::string err;
  CHECK(cv::json::parse(text, v, err));
  CHECK(v.type() == cv::json::Value::Type::Object);
  CHECK(v.find("name")->stringValue() == "报告.docx");
  CHECK(v.find("size")->numberValue() == 2048.0);
  CHECK(v.find("tags")->items().size() == 2);
  CHECK(v.find("ok")->boolValue() == true);
  CHECK(v.find("x")->isNull());
  CHECK(v.find("missing") == nullptr);

  cv::json::Value out = cv::json::Value::object();
  out.set("id", 7);
  out.set("instant", true);
  out.set("msg", "中文 \"引号\" \\ 反斜杠");
  cv::json::Value arr = cv::json::Value::array();
  arr.push_back(cv::json::Value(1));
  arr.push_back(cv::json::Value("two"));
  out.set("items", arr);
  std::string dumped = cv::json::dump(out);

  cv::json::Value back;
  CHECK(cv::json::parse(dumped, back, err));
  CHECK(back.find("id")->numberValue() == 7.0);
  CHECK(back.find("instant")->boolValue() == true);
  CHECK(back.find("msg")->stringValue() == "中文 \"引号\" \\ 反斜杠");
  CHECK(back.find("items")->items().size() == 2);

  cv::json::Value bad;
  CHECK(!cv::json::parse("{\"a\":1", bad, err));
}

void testStore() {
  fs::path root = fs::temp_directory_path() / "cv_store_test";
  fs::remove_all(root);

  cv::ContentStore store(root.string());
  std::string err;
  CHECK(store.init(err));

  CHECK(!cv::ContentStore::validHash("../etc/passwd"));
  CHECK(!cv::ContentStore::validHash("abc"));

  std::string data = "hello cloudvault";
  std::string hash = cv::Sha256::of(data);
  CHECK(store.put(hash, data, err));
  CHECK(store.exists(hash));
  std::string out;
  CHECK(store.get(hash, out, err));
  CHECK(out == data);

  // 分块：chunkSize=4 -> "abcd","efgh","ij"
  std::string payload = "abcdefghij";
  std::vector<std::string> hashes;
  std::vector<std::size_t> sizes;
  std::string fileHash;
  CHECK(store.putChunked(payload, 4, hashes, sizes, fileHash, err));
  CHECK(hashes.size() == 3);
  CHECK(sizes.size() == 3);
  CHECK(sizes[0] == 4 && sizes[2] == 2);

  std::string joined;
  CHECK(store.getChunked(hashes, joined, err));
  CHECK(joined == payload);

  // 空文件也要能存
  std::vector<std::string> emptyHashes;
  std::vector<std::size_t> emptySizes;
  std::string emptyHash;
  CHECK(store.putChunked("", 4, emptyHashes, emptySizes, emptyHash, err));
  CHECK(emptyHashes.size() == 1);

  fs::remove_all(root);
}

}  // namespace

int main() {
  testSha256();
  testJson();
  testStore();
  if (g_failed == 0) {
    std::printf("全部自检通过\n");
    return 0;
  }
  std::printf("失败 %d 项\n", g_failed);
  return 1;
}
