# sufkit 中文概览

`sufkit` 是一个面向基因组序列的 C++17 后缀索引库和命令行工具，支持：

- divsufsort32/64 后缀数组构建；
- CaPS-SA 32/64 位共享内存并行构建；
- SDSL Huffman、balanced 和 DNA EPR compressed suffix array；
- exact count、equal range 和 locate；
- ISA、LCP、CHILD 与 suffix-link 右极大精确匹配搜索；
- 正式的双侧极大 MEM 与 reference-MAM（参考中唯一、query 可重复）搜索；
- 可选的文本位置采样 SA，保持完整 exact count/locate 和右极大匹配结果；
- 可选的 Sapling 风格分段线性 learned index；
- FASTA/FASTA.gz、多 contig、正向、反向互补和双链查询；
- 自包含、带版本和 CRC 校验的 `.sufidx` 文件；
- 确定性 benchmark 和 MUMmer4 黑盒结果对照。

英文文档是详细接口与实现契约的权威版本。中文文档提供快速上手、索引选择和性能结论概览。

`RightMaximal*` 保留历史兼容的较弱契约；需要正式左、右双侧极大保证时使用
0.3.0 开发中的 `Mem*` API。`Mam*` 进一步要求匹配串在全部 reference contig
中只出现一次，但允许它在 query 中重复；严格 MUM 尚未实现。

[中文文档导航](docs/zh-CN/README.md) · [英文完整文档](docs/README.md) ·
[贡献指南](CONTRIBUTING.md)

0.2.0 已统一 C++ 命名：函数使用
`PascalCase`，枚举值使用 `kPascalCase`。0.1.x 调用方请参考
[API 命名迁移指南](docs/development/api-naming-migration-0.2.0.md)。公共头文件
路径、CMake target、CLI 与 `.sufidx` 格式没有因此改变。

## 当前版本口径

当前发布版本是 `0.2.0`。CaPS、采样 SA、balanced/EPR FM-index、FM batch
count 和 Sapling PWL 均已随 0.2.0 发布；其中采样 SA 和 Sapling PWL 仍是
默认关闭的实验性能力。0.2.0 相对 0.1.x 是源码不兼容升级，旧调用方需要按
命名迁移指南修改源码。

默认选择保持保守：

- 普通 SA 构建使用 divsufsort；只有逻辑文本至少 1 GiB、线程数大于 1 且 CaPS 可用时，`auto` 才选择 CaPS。
- SA 默认构建 `SA+ISA+LCP`，右极大匹配自动使用 suffix-link；CHILD 只在显式请求时使用。
- 采样 SA 默认关闭。`K>1` 主要减少最终索引内存和文件大小，不降低底层完整 SA 构建的峰值内存；采样右极大匹配要求 `min_length >= K`。
- FM-index 默认使用 Huffman；EPR 适合查询速度优先且能接受更大索引的场景。
- Sapling PWL 默认关闭，因为其收益与数据重复结构和查询负载有关。

## 快速构建

当前验证环境是支持 SSE4.2 和 POPCNT 的 x86_64 Linux/WSL；不要求 AVX2 或
AVX-512。该编译选项仅用于 sufkit 私有实现，不会传播给 CMake 使用者。

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
```

构建并查询 FM-index：

```bash
./build/release/sufkit build --type fm \
  --input reference.fa.gz --output reference.fm.sufidx

./build/release/sufkit query --index reference.fm.sufidx \
  --pattern ACGTACGT --strand both
```

构建 SA 并搜索右极大精确匹配：

```bash
./build/release/sufkit build --type sa \
  --input reference.fa.gz --output reference.sa.sufidx

./build/release/sufkit right-maximal --index reference.sa.sufidx \
  --query queries.fa.gz --min-length 20 --strand both

./build/release/sufkit mem --index reference.sa.sufidx \
  --query queries.fa.gz --min-length 20 --strand both

./build/release/sufkit mam --index reference.sa.sufidx \
  --query queries.fa.gz --min-length 20
```

所有公开坐标都是 0-based、contig-local。exact pattern 只接受 A/C/G/T；
右极大、MEM 和 MAM query 中的其他字符会成为 hard break。

下一步建议阅读：

1. [中文快速入门](docs/zh-CN/quickstart.md)
2. [如何选择索引](docs/zh-CN/choosing-an-index.md)
3. [性能结论概览](docs/zh-CN/benchmark-summary.md)
4. [英文 C++ 使用指南](docs/user-guide/cpp-workflows.md)
5. [英文内部架构](docs/development/architecture.md)
