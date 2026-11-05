# 搜索底层选型复核（2026-11-03）

结论：局部复用开源 NTFS 元数据模块，Flowtary 自己实现调度、持久化、查询规则、IPC 和资源门槛。首选验证 GoZ 的 goz-core/goz-winfs；不整体接入其守护进程，不把它视为已经证明低系统成本的引擎。fd 不再承担全盘建库或查询回退。C 卷不支持 FILE_LAYOUT，代码现含受限的 USN+MFT FILE_NAME fallback 与硬链接计数审计；目前仅通过离线测试，未通过真实卷运行，不能据此启用全盘索引。

## 历史失败与适用结论

- `c1b1c2c` / `99b52ea`：旧 alt-search 使用 jwalk 目录遍历。常驻单例和缓存可以减少重复初建，但不能消除目录枚举与逐项元数据请求。与此次避免大量文件系统操作的目标不符。
- `f6c5b5b`：UFFS 改造记录了 uffsd 约 700 MB–1 GB，并记录适配层退出时关闭后端、后续重新提权的问题。源码还有 `Instant::now().elapsed()` 被当作活动时钟，不能据此可靠计算空闲时长。历史记录的内存比较不足以说明 UFFS 的整机成本不合格，也没有证明其存在此次内核增长。当前 uffs-core/uffs-mft 与 Polars 耦合明显，完整接入会引入超出所需的结构与生命周期。
- `c054769` 之后：自有 fd 建库及交互查询可能重叠，全盘监听又可能触发重建。用户暂停/恢复实测支持扫描是 System 句柄及内核分配增长的触发活动；具体泄漏对象与驱动责任仍未由调用栈证据确认。

因此，不能把这些经历概括为“开源不可靠”，也不能用新候选的低 RSS 或快速演示替代整机验证。

## GitHub 源码候选

本次复核沿用已保存的固定提交源码，并通过 raw.githubusercontent.com 再次读取 GoZ tail、UFFS core manifest 和 Everyfind daemon。GitHub 元数据 API 本次返回限流，以下不是对所有项目最新 HEAD 的完整审计。

- [GoZ](https://github.com/mustafaahci/goz/tree/8c6a7b1562bb1eea46dd67d462805b49b9a1f152)，MIT。纯解析/索引 core 与 Win32 层分离，便于限范围复用。注意 upstream daemon 有自动创建/调整 USN journal、50 ms 空闲轮询和工作集调整；cursor 模块明确说明当时未持久化完整索引。这些行为不直接继承。首选理由是模块边界适合审计和替换，不是已经测出它比其他项目省资源。
- [UFFS](https://github.com/skyllc-ai/UltraFastFileSearch/tree/d02001efb60734cc7a0a3eaab32d71eadd857847)。core/mft 的 Polars 依赖使完整移植偏重；保留 NTFS 实现参考价值，不再次整体接入。历史版本 workspace license 为 MPL-2.0，实际复用前须核对所选版本、文件及依赖许可。
- [Everyfind](https://github.com/kyo5uke/everyfind/tree/117d53c67fd94d386edd02e0c3e9ba358a409735)，MIT/Apache-2.0。MFT 枚举、USN、快照恢复值得参考；README 明确该版本一个记录只索引一个名称，不能直接满足完整硬链接路径需求。可作备用对照实现。
- [AllTheThings](https://github.com/Swatto86/AllTheThings/tree/71eb7b5c0dd951c338ff09edd420dd288a23974b)，MIT。完整硬链接、快照与 USN 恢复有参考价值，但还包含完整产品/服务，并为大小及时间戳补读变更文件的 MFT 记录。Flowtary 的首期名称索引无需照搬这些补读。
- [mft](https://github.com/omerbenamram/mft)，MIT/Apache-2.0。适合原始 MFT 解析参考；它不是包含在线 journal、一致快照、调度和查询的完整索引服务，直接采用仍需自行承担较多底层工作。

## Flowtary 应拥有的机制

1. 本机 NTFS：有界、串行的卷级元数据初建，USN 增量；不逐目录全盘递归，不逐文件补读内容。优先 FSCTL_QUERY_FILE_LAYOUT；若不支持，则对 FSCTL_ENUM_USN_DATA 每个 FRN 批量补取单个 FILE_RECORD，并按 base/extension 名称数量审计。fallback 仍须经过真实卷完整性与整机资源测量。
2. 查询：读已发布内存索引；预建候选 posting、有限 top-K、时间预算与取消。广泛查询可以明确返回部分结果，不能把截断结果称为完整命中。
3. workshop：同一元数据目录上的独立规则，`C:\workshop\` 子树且扩展名 `.exe`，目录边界排除 `C:\workshop-old`；筛选、版本排序和优先级属于 Flowtary，不委托 Everything。
4. 快速应用入口：小型 Start Menu/注册应用 catalog，先于全盘索引可用；不启动递归 Program Files 扫描。真正第一次冷建时，任意 workshop 文件仍不能在尚未读取前保证立即可搜；后续依靠持久化专用目录与增量恢复。
5. 持久化：索引与卷身份、journal ID、USN 游标一起原子保存；断档标 stale、停止自动重试，受控重建不回退到 walker。
6. 权限：若采用需要卷读取权限的接口，权限隔离在最小服务/辅助进程中，UI 保持普通用户权限；服务句柄和内存也计入整机开销。非 NTFS/网络卷先明确支持边界，不静默切换扫描方式。

## 当前实现及证据边界

`indexer/` 已有局部复用 GoZ 的实验原型，release 编译成功；Rust 离线 21 项测试通过，C++ 侧快照路由、IPC JSON 解析和 UI dispatcher 测试也通过。持久查询协议已接入 C++ d/f，300 次 fixture 查询的 p50/p95 为 10.144/16.159 ms，六项整机数据见 `docs/INDEXER_CPP_INTEGRATION.md`。C 卷真实 attribute-list 扩展记录、完整快照 writer、持续 USN 生命周期和长时资源验收仍未完成。

此前 10 万/30 万条合成数据结果仅证明内存结构和查询原型可行，不能作为真实卷读取速度、系统成本或无内核泄漏的证据。随后按用户明确授权，已完成小 VHD 初建/USN/硬链接验证和 C 卷有限分页探测；尚未执行完整物理卷采集。

发布前必须在可回滚隔离环境实测：整机提交量、可用内存/换页压力、System 句柄、磁盘活动、分页/非分页池、CPU；覆盖初建、暂停恢复、空闲增量、批量变更、重启恢复和 journal 断档。过程需要同时记录绝对值、增量和运行后是否回落/趋稳。驱动层异常不能靠结束用户态进程保证回收，也不能承诺任何 FSCTL 绝对不触发它。
