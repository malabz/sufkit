# Benchmark 结论概览

当前结论来自固定 seed 的 synthetic smoke/quick 实验，不代表所有真实基因组和硬件。standard、full 和大型真实基因组实验没有自动执行。

- SA 构建：CaPS32 在 64 MiB、8 线程时相对 divsufsort32 为 1.48x，但峰值 RSS 约 3.01x；1 MiB 输入上并行开销使 CaPS 明显更慢。
- 采样 SA：1 MiB smoke 中 K=8 的 full-ESA 文件缩小 82.3%，divsufsort worker 峰值 RSS 降低 77.8%；CaPS 仍要先持有完整 SA/LCP，因此构建峰值收益较弱。这只是单次 smoke 的内存形状证据。
- exact SA 搜索：LCP-aware binary 在 mixed 数据上较稳定；Sapling PWL 在部分 mixed/repeat-rich workload 中有收益；CHILD 当前存在负优化。
- right-maximal exact match：suffix-link 是主要加速来源和默认算法。Sapling fallback 在多数 quick 组合上有收益，但 repeat-rich/min-20 有轻微回退，因此保持显式实验选项。
- FM-index：Huffman 空间最优且为默认；EPR 查询更快但约使用 3 倍 Huffman 序列化空间并增加加载成本；balanced 当前是负向对照。
- MUMmer4：用于可比子集的结果正确性对照；其外部 `load+query` 时间不能与 sufkit 进程内 query-only 时间直接比较。

详细命令、环境、fingerprint、checksum 和限制见[英文 benchmark 总结](../benchmarks/README.md)及其版本化结果报告。
