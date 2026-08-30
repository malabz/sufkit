# 如何选择索引

先根据任务选择数据结构，再根据空间和速度选择后端：

| 任务 | 建议起点 |
|---|---|
| 压缩空间下的 exact count | Huffman FM-index |
| FM 查询速度优先 | EPR FM-index，但需要接受更大的索引和加载成本 |
| right-maximal exact match 搜索 | 默认 SA+ISA+LCP 与 suffix-link |
| 正式 MEM 搜索 | LCP+auto-skip；完整和采样 SA 均支持 |
| reference-MAM | 完整 SA；要求匹配串在联合 reference 中唯一 |
| 广义 `(l,c)-SMEM` | 完整 SA；返回每个 reference 坐标及完整 occurrence 数 |
| 严格 MUM | 完整 SA；同时要求 reference 和当前 query record 中唯一 |
| 最低 standalone SA 常驻内存 | Low-memory：完整 SA+byte-coded LCP |
| 直接访问 SA row 或开发 ESA 算法 | standalone SA |
| 大型参考的多线程 SA 构建 | CaPS，先评估峰值内存 |
| 降低最终 standalone SA 大小 | 文本位置采样 SA，先确认短模式和 right-maximal exact match 阈值限制 |
| learned exact lookup 研究 | 显式启用 Sapling PWL |
| CHILD interval 研究 | 显式构建 child/full，不进入自动路径 |

CaPS 的 `auto` 条件是逻辑文本至少 1 GiB、线程数大于 1 且编译时启用
CaPS。它在当前 64 MiB quick 实验中八线程快于 divsufsort32，但峰值内存约为
后者三倍；这一结果不能直接外推成人类全基因组阈值。Low-memory 只压低最终
常驻索引，不会消除 CaPS 构建期的完整 SA/LCP、工作数组和分区元数据；构建
内存受限时应单独测量 CaPS 峰值。

默认 SA acceleration 是 `lcp_suffix_link`，持久化 SA+ISA+LCP。CHILD 保留给 suffix-tree 风格导航和显式算法消融；exact 与 maximal-match 自动选择都不会消费 CHILD。

`--sa-width` 与 `--sa-storage-width` 不是一回事：前者决定构建器用 32
还是 64 位，后者决定最终 SA 及 auxiliary 优先使用 native32、split40、
split48 或 native64；需要 one-past row 标记的 auxiliary 可按需单独提升。
判断依据是“碱基 + 每条 contig 的 separator + sentinel”的完整
逻辑符号数。即使 divsufsort 需要 64 位构建，只要最大位置仍可表示，最终
索引仍可在完整校验后压回更窄的存储。

Fast 是默认 profile：能用 native32 时使用 32 位，否则自动使用 native64，
并持久化原生 raw LCP，以保留 ISA/suffix-link 的随机访问性能。Low-memory
固定保留完整 SA+byte-coded LCP，移除常驻 ISA、CHILD 和 PWL，并自动选择
最窄的 32/40/48/64 位存储；它目前要求 K=1。split40 是 low32+high8 两个
数组，split48 是 low32+high16，避免结构体 padding 到 8 字节。Low-memory
LCP 使用一字节主平面和长值 anchor。两个 profile 的 MEM auto 都使用
LCP+auto-skip；Fast 的 reference-MAM、SMEM 和 MUM auto 使用 suffix-link，
Low-memory 则使用 LCP。

`--sa-sampling-rate K` 会只保留文本位置能被 K 整除的 suffix。它可以让
最终 SA、ISA、LCP、CHILD 和序列化文件大约按 K 缩小，但底层仍先构建完整
SA，所以不能用来解决构建峰值内存。采样索引的 `count/locate` 会恢复全部
结果，`EqualRange` 不可表示为单一区间，right-maximal 和 MEM 要求
`min_length >= K`。reference-MAM、SMEM 和 MUM 只支持 K=1。

split40/48 已通过边界和小数据差分测试，但还没有完成超过 `2^32` 逻辑符号
的真实规模实验，也不能据此宣称已经在时间和内存上全面超过 MUMmer4。
这些区间暂时应视为实验能力。

Huffman 是 FM 默认后端。EPR 在当前合成 DNA benchmark 中 count/locate 更快，但序列化大小和加载时间显著增加；balanced 当前没有体现收益。

详细决策依据见[英文索引选择指南](../getting-started/choosing-an-index.md)。
