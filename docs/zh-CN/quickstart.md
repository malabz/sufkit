# 中文快速入门

环境要求：支持 SSE4.2 和 POPCNT 的 x86_64 Linux/WSL、GCC 或 Clang、CMake
3.20+、C++17 和 ZLIB；不要求 AVX2 或 AVX-512。SDSL、libdivsufsort、CaPS、
ParlayLib 与 kseq 已经随仓库固定，不需要联网下载。

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
```

构建默认 Huffman FM-index：

```bash
./build/release/sufkit build --type fm \
  --input reference.fa.gz --output reference.fm.sufidx
```

进行 exact count 和 locate：

```bash
./build/release/sufkit query --index reference.fm.sufidx \
  --pattern ACGTACGT --count-only

./build/release/sufkit query --index reference.fm.sufidx \
  --pattern ACGTACGT --strand both --max-hits 100
```

构建默认 `SA+ISA+LCP` 并搜索 right-maximal exact match：

```bash
./build/release/sufkit build --type sa \
  --input reference.fa.gz --output reference.sa.sufidx

./build/release/sufkit right-maximal --index reference.sa.sufidx \
  --query queries.fa.gz --min-length 20 --strand both
```

需要正式双侧极大 MEM 或 reference 中唯一的 MAM 时：

```bash
./build/release/sufkit mem --index reference.sa.sufidx \
  --query queries.fa.gz --min-length 20 --strand both
./build/release/sufkit mam --index reference.sa.sufidx \
  --query queries.fa.gz --min-length 20
```

对大型参考显式使用 CaPS：

```bash
./build/release/sufkit build --type sa \
  --input large-reference.fa.gz --output large.sa.sufidx \
  --sa-backend caps --threads 16
```

重要语义：

- FASTA reference 会规范化成 A/C/G/T/N。
- exact pattern 只允许 A/C/G/T。
- right-maximal、MEM 和 MAM query 的非 A/C/G/T 字符是 hard break。
- N、contig separator 和 sentinel 都不能被匹配跨越。
- 坐标是 0-based、contig-local；CLI exact end 是 exclusive。
- `--max-hits` 或 `--max-matches` 截断输出，但结果对象仍报告完整总数。

完整说明见[英文快速入门](../getting-started/quickstart.md)。
