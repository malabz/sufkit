# sufkit 中文概览

`sufkit` 是一个面向基因组序列的 C++17 后缀索引库和命令行工具，支持：

- divsufsort32/64 后缀数组构建；
- CaPS-SA 32/64 位共享内存并行构建；
- SDSL Huffman、balanced 和 DNA EPR compressed suffix array；
- exact count、equal range 和 locate；
- ISA、Kasai LCP、CHILD 与 suffix-link MEM 搜索；
- 可选的文本位置采样 SA，保持完整 exact count/locate 和 MEM 结果；
- 可选的 Sapling 风格分段线性 learned index；
- FASTA/FASTA.gz、多 contig、正向、反向互补和双链查询；
- 自包含、带版本和 CRC 校验的 `.sufidx` 文件；
- 确定性 benchmark 和 MUMmer4 黑盒结果对照。

英文文档是详细接口与实现契约的权威版本。中文文档提供快速上手、索引选择和性能结论概览。

[中文文档导航](docs/zh-CN/README.md) · [英文完整文档](docs/README.md) ·
[贡献指南](CONTRIBUTING.md)

## 当前版本口径

已发布版本是 `0.1.1`。当前 `main` 还包含尚未重新发布的 CaPS、采样
SA、balanced/EPR FM-index、FM batch count 和 Sapling PWL 等能力，因此这些
功能在文档中统一标记为 `Unreleased`，不会倒写成已经发布的 0.1.1 功能。

默认选择保持保守：

- 普通 SA 构建使用 divsufsort；只有逻辑文本至少 1 GiB、线程数大于 1 且 CaPS 可用时，`auto` 才选择 CaPS。
- SA 默认构建 `SA+ISA+LCP`，MEM 自动使用 suffix-link；CHILD 只在显式请求时使用。
- 采样 SA 默认关闭。`K>1` 主要减少最终索引内存和文件大小，不降低底层完整 SA 构建的峰值内存；采样 MEM 要求 `min_length >= K`。
- FM-index 默认使用 Huffman；EPR 适合查询速度优先且能接受更大索引的场景。
- Sapling PWL 默认关闭，因为其收益与数据重复结构和查询负载有关。

## 快速构建

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

构建 SA 并搜索 MEM：

```bash
./build/release/sufkit build --type sa \
  --input reference.fa.gz --output reference.sa.sufidx

./build/release/sufkit mem --index reference.sa.sufidx \
  --query queries.fa.gz --min-length 20 --strand both
```

所有公开坐标都是 0-based、contig-local。exact pattern 只接受 A/C/G/T；MEM query 中的其他字符会成为 hard break。

下一步建议阅读：

1. [中文快速入门](docs/zh-CN/quickstart.md)
2. [如何选择索引](docs/zh-CN/choosing-an-index.md)
3. [性能结论概览](docs/zh-CN/benchmark-summary.md)
4. [英文 C++ 使用指南](docs/user-guide/cpp-workflows.md)
5. [英文内部架构](docs/development/architecture.md)
