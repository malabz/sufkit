# 如何选择索引

先根据任务选择数据结构，再根据空间和速度选择后端：

| 任务 | 建议起点 |
|---|---|
| 压缩空间下的 exact count | Huffman FM-index |
| FM 查询速度优先 | EPR FM-index，但需要接受更大的索引和加载成本 |
| right-maximal exact match 搜索 | 默认 SA+ISA+LCP 与 suffix-link |
| 正式 MEM 搜索 | 默认 SA+ISA+LCP 与 suffix-link；完整和采样 SA 均支持 |
| reference-MAM | 完整 SA；要求匹配串在联合 reference 中唯一 |
| 直接访问 SA row 或开发 ESA 算法 | standalone SA |
| 大型参考的多线程 SA 构建 | CaPS，先评估峰值内存 |
| 降低最终 standalone SA 大小 | 文本位置采样 SA，先确认短模式和 right-maximal exact match 阈值限制 |
| learned exact lookup 研究 | 显式启用 Sapling PWL |
| CHILD interval 研究 | 显式构建 child/full，不进入自动路径 |

CaPS 的 `auto` 条件是逻辑文本至少 1 GiB、线程数大于 1 且编译时启用 CaPS。它在当前 64 MiB quick 实验中八线程快于 divsufsort32，但峰值内存约为后者三倍；这一结果不能直接外推成人类全基因组阈值。

默认 SA acceleration 是 `lcp_suffix_link`，持久化 SA+ISA+LCP。CHILD 保留给 suffix-tree 风格导航、显式 MEM/MAM 消融和重复区研究，但 exact、right-maximal、MEM/MAM 自动选择都不会消费 CHILD。

`--sa-sampling-rate K` 会只保留文本位置能被 K 整除的 suffix。它可以让
最终 SA、ISA、LCP、CHILD 和序列化文件大约按 K 缩小，但底层仍先构建完整
SA，所以不能用来解决构建峰值内存。采样索引的 `count/locate` 会恢复全部
结果，`EqualRange` 不可表示为单一区间，right-maximal 和 MEM 要求
`min_length >= K`。reference-MAM 只支持 K=1。

Huffman 是 FM 默认后端。EPR 在当前合成 DNA benchmark 中 count/locate 更快，但序列化大小和加载时间显著增加；balanced 当前没有体现收益。

详细决策依据见[英文索引选择指南](../getting-started/choosing-an-index.md)。
