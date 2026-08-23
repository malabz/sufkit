# 如何选择索引

先根据任务选择数据结构，再根据空间和速度选择后端：

| 任务 | 建议起点 |
|---|---|
| 压缩空间下的 exact count | Huffman FM-index |
| FM 查询速度优先 | EPR FM-index，但需要接受更大的索引和加载成本 |
| MEM 搜索 | 默认 SA+ISA+LCP 与 suffix-link |
| 直接访问 SA row 或开发 ESA 算法 | standalone SA |
| 大型参考的多线程 SA 构建 | CaPS，先评估峰值内存 |
| learned exact lookup 研究 | 显式启用 Sapling PWL |
| CHILD interval 研究 | 显式构建 child/full，不进入自动路径 |

CaPS 的 `auto` 条件是逻辑文本至少 1 GiB、线程数大于 1 且编译时启用 CaPS。它在当前 64 MiB quick 实验中八线程快于 divsufsort32，但峰值内存约为后者三倍；这一结果不能直接外推成人类全基因组阈值。

默认 SA acceleration 是 `lcp_suffix_link`，持久化 SA+ISA+LCP。CHILD 保留给 suffix-tree 风格导航、未来 MUM/MAM 和重复区研究，但当前 exact 和 MEM 自动选择都不会消费 CHILD。

Huffman 是 FM 默认后端。EPR 在当前合成 DNA benchmark 中 count/locate 更快，但序列化大小和加载时间显著增加；balanced 当前没有体现收益。

详细决策依据见[英文索引选择指南](../getting-started/choosing-an-index.md)。
