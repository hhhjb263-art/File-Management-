# CloudVault 服务端验收测试脚本

本目录下的脚本用于在 **Linux/Debian** 上对真实运行的 `cloudvault-server` 做端到端验收，
补充 C++ 单元自检（`cv_tests`）覆盖不到的网络层、鉴权与磁盘落盘行为。

> **重要：本机（Windows）无法运行服务端**（POSIX socket + SQLite3），
> 因此这些脚本**必须在 Linux/Debian 上执行**。Windows 上只能做语法检查
> （`python3 -m py_compile` / `bash -n`），不能真正跑通。

## 前置条件

- 已在 `server/` 目录构建：`cmake -S . -B build && cmake --build build -j`，
  得到 `build/cloudvault-server`（所有脚本用 `CV_SERVER_BIN` 指定其路径，默认 `./build/cloudvault-server`）。
- 运行环境具备：`curl`、`sha256sum`、`od`、`dd`、`find`、`cmp`、`sed`、`seq`、`python3`（仅 keepalive 需要）。
- 若脚本无可执行位，请先赋权：
  ```bash
  chmod +x server/tests/*.py server/tests/*.sh
  ```

> **依赖的待落地修复**：下面部分断言是针对正在修复的 P2 项编写的回归用例，
> 在修复落地前会 FAIL（属预期，正是回归信号）：
> - `constantTimeEqual` 长度边界（含 256 倍数）→ `auth_bypass_test.sh` 长度边界段；
> - `Transfer-Encoding: chunked` 显式拒绝 → `keepalive_test.py` 用例 5(a)；
> - 加密 blob 文件名带 `.enc` 后缀（读取先 `.enc` 再回退明文）→ 两个加密脚本；
> - `--http=off` 日志顺序（TLS 成功之后再提示“仅保留 HTTPS”）→ `http_off_test.sh` 检查 B。

## 五个脚本

### 1. `keepalive_test.py` —— keep-alive / pipelining / 分帧健壮性

覆盖五类边界：

1. **同段两个 GET**：一次 `sendall` 发两个 `GET /healthz` → 各自返回一个响应，首响应 `Connection: keep-alive`；
2. **`Content-Length: N` 的 body 边界**：带 5 字节 body 的 `POST /api/v1/files` 紧跟 GET；
3. **`Content-Length: 0` 的边界**：零长 body 紧跟 GET；
4. **单连接请求上限（200）**：逐个发 201 个 `GET /healthz`，前 200 个正常、第 200 个响应标 `Connection: close`、
   第 201 个读取失败（连接已关闭）；
5. **分帧严格性（raw socket）**：(a) `Transfer-Encoding: chunked` 不得当定长帧接受（非 2xx 或直接关闭）；
   (b) 重复 `Content-Length` 报告实际行为，仅当出现“请求走私式错位”才失败。

每个用例打印「期望 vs 实得」，任一失败 `exit 1`，结尾 `RESULT: PASS/FAIL`。

```bash
python3 keepalive_test.py
python3 keepalive_test.py --port 8080 --token SECRET123   # 服务端启用鉴权时
```

> 服务端启用鉴权但未传 `--token` 时，用例 2/3/5 的 POST 会得到 401 并断连 → 用例失败；
> `/healthz` 始终豁免。

### 2. `auth_bypass_test.sh` —— 鉴权绕过清单 + 长度边界回归

- **主用例（19 项）**：15 条恶意/畸形请求（应 401）+ 4 条合法请求（应 200）。覆盖缺失/空 token、
  `bearer`/`BeArEr` 大小写变体、错误/引号/尾空格 token、路径大小写变体（`/API/...`、`/HEALTHZ`）、
  `HEAD`/`OPTIONS`、`/healthz/`、`//healthz`、`X-CV-Token` 兼容头、`/healthz?x=1` 豁免。
- **长度边界回归（4 项）**：脚本自动另起**独立实例**（token = `A`×300，默认端口 8081），断言
  `A`×44（真前缀，Δlen=256，修复前会误判 200）**必须 401**、`A`×300→200、`A`×45→401、`A`×299→401。

**前置：主用例需服务端以 `--auth-token=$CV_TOKEN` 启动**：

```bash
./build/cloudvault-server --data-dir /tmp/cv-auth --port 8080 --auth-token SECRET123
```

```bash
bash auth_bypass_test.sh
CV_TOKEN=mysecret CV_BASE=http://127.0.0.1:8443 bash auth_bypass_test.sh
CV_TEST_LEN_BOUNDARY_PORT=8091 CV_SKIP_LEN_BOUNDARY=1 bash auth_bypass_test.sh   # 跳过长度边界段
```

汇总 `PASS/FAIL/SKIP`；存在 FAIL 时 `exit 1`。

### 3. `encryption_roundtrip.sh` —— 静态加密往返 + 落盘格式

1. 非法加密配置（缺密钥文件 / 密钥长度非法）应启动失败；
2. 上传随机 300KB → 下载 → `cmp` 往返一致；
3. 落盘为密文：`blobs/` 下存在 `*.enc`、其前 4 字节为 `CVB1`，且**不存在无 `.enc` 后缀的明文 blob**；
4. 去重键仍为明文 sha256，重复上传命中秒传（`"instant":true`）；
5. Range 下载（`bytes=100-199`）走分块拼装 + 解密，往返一致。

```bash
CV_SERVER_BIN=./build/cloudvault-server bash encryption_roundtrip.sh
```

### 4. `encryption_migration_test.sh` —— **加密迁移 / 混合库共存**（新增）

验证“`.enc` 与旧明文库共存”这一设计取舍，分三阶段（各自独立重启 + `trap` 清理）：

1. **明文库**：不带 `--data-key` 启动 → 上传 fileA（明文 blob）；
2. **加密开启**：带 `--data-key` 重启 → ① 旧 fileA 仍能下载且 `cmp` 一致（回退明文）；
   ② 新上传 fileB → `*.enc`；③ 旧明文 blob **仍保留**；
3. **无密钥读密文**：再不带 `--data-key` 重启 → 无密钥读 fileB(`.enc`) 必须是**干净失败**
   （5xx / 连接错误，非 2xx），且**不得泄漏明文**（响应体不等于原文）。

```bash
CV_SERVER_BIN=./build/cloudvault-server bash encryption_migration_test.sh
```

### 5. `http_off_test.sh` —— **启动期行为 `--http=off`**（新增）

- **A**：`--http=off` 且未配 `--tls-port` → 非零退出 + 明确错误信息；
- **B**：`--http=off` + 正确 `--tls-port/--tls-cert/--tls-key` → 正常运行；
  且“明文 HTTP 已关闭 / 仅保留 HTTPS”类提示必须出现在 **TLS 监听成功之后**
  （顺序反了算 FAIL；用 `openssl req -x509 ...` 现场生成自签证书，用完删除）；
- **C**：`--http=off` + `--tls-port` 但缺证书/私钥 → 非零退出（不静默降级）。

```bash
CV_SERVER_BIN=./build/cloudvault-server bash http_off_test.sh
```

## 可覆盖的环境变量

| 变量 | 用于 | 默认 |
| --- | --- | --- |
| `CV_SERVER_BIN`（兼容 `CV_SERVER`） | 2、3、4、5 | `./build/cloudvault-server` |
| `CV_TOKEN` / `CV_BASE` | 2 | `SECRET123` / `http://127.0.0.1:8080` |
| `CV_TEST_LEN_BOUNDARY_PORT` / `CV_TEST_LEN_BOUNDARY_DIR` / `CV_SKIP_LEN_BOUNDARY` | 2 | `8081` / `/tmp/cv-len-boundary` / 未设 |
| `CV_PORT` / `CV_DATA_DIR` / `CV_KEY_FILE` / `CV_BAD_KEY_FILE` | 3 | `8080` / `/tmp/cv-enc-test` / `/tmp/cv-data.key` / `/tmp/cv-bad.key` |
| `CV_MIG_PORT` / `CV_MIG_DATA_DIR` / `CV_MIG_KEY_FILE` / `CV_MIG_TOKEN` | 4 | `8082` / `/tmp/cv-mig-test` / `/tmp/cv-mig-data.key` / `s3cr3t` |
| `CV_HTTP_OFF_TLS_PORT` / `CV_HTTP_OFF_DIR` | 5 | `8443` / `/tmp/cv-http-off` |

## 退出码约定

所有脚本：全部通过 → `0`；存在失败 → `1`；找不到可执行文件（加密/迁移/http_off）→ `2`。

## 仍需人工验证（脚本无法自动化的部分）

1. **`statusText` 的 401/403 reason phrase**：脚本只断言状态码，不看状态行短语。
   需人工 `curl -i` 确认 401 → `Unauthorized`、403 → `Forbidden`（否则仍是 `Unknown`）。
2. **`.enc` 与旧明文库时间窗内的并发访问**：迁移脚本为顺序重启验证；
   服务运行中“同时存在明文 blob 与 `.enc` blob 且并发读写同一 hash”的时序/竞争需人工或专项压测。
3. **完整加密数据目录的备份/恢复/密钥轮换**：超出本次范围。
4. **文档一致性（P2）**：如 `/api/v1/tree` 是否被注释为“不鉴权”等，属静态审查，非运行时。
