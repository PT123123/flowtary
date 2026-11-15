# 本机索引验证记录（2026-11-03）

用户明确授权在当前物理机分阶段测试，接受可能需要重启。因此，本次没有停留在 VM 条件缺失，也没有放开无限量扫描：先用新建小 VHD 验证完整功能，再用明确限定页数的 C 卷元数据探测验证兼容性。旧异常内核基线完整保留在采样中。

旧版 Flowtary 进程已停止；另确认 HKCU Run 的 `Flowtary` 仍指向 `C:\workshop\Flowtary-1.1.25.10\flowtary-1.1.25.10.exe`。已备份到 `logs/indexer/old-flowtary-startup-backup-20261104.json` 并暂时删除这一项，防止用户重启后重启旧扫描器。新版本验证并部署后，应按备份恢复用户原有的开启自启动偏好。

## 后续无卷访问宿主基线（2026-11-04）

为判断后续隔离测试是否有足够物理内存与提交空间，执行 10 秒 `metrics` 观测；不打开卷、不枚举文件。样本和报告在 `logs/indexer/current-host-baseline-20261104-01/`。有效 11 个样本中，提交量约 40.21 GiB、相对阶段首样本峰值增长 0 MiB；可用内存最低 10.24 GiB、压力比 67.56%–67.60%；System 句柄约 2,522,358 起始、峰值 +5；分页池约 9.08 GiB 起始、峰值 +0.34 MiB，非分页池约 5.27 GiB 起始、峰值 +0.59 MiB；机器 CPU 加权平均 2.55%，磁盘 busy 1.36%，整机读写约 0.48 MiB/s；计数器错误 0。虽然内存/提交门槛有余量，System 句柄与两类池的绝对值仍违反 native-host 门槛，不允许因此放行物理卷扫描。

本会话 shell 当前未提升权限；既有 native collector 需要管理员令牌。此前管理员 UAC 探针被取消，因此本轮仅记录无卷基线，没有重新触发提权，也没有挂载 VHD 或访问 C 卷。

### 服务化前的当前主机六项基线（2026-11-05）

运行目录：`logs/indexer/service-architecture-baseline-20261105/`。只运行 15 秒 `metrics`，不打开卷、不枚举文件、不申请 UAC。有效 machine-observation 为 15.02 秒、16 个样本，计数器错误 0。提交量约 46.38 GiB，窗口峰值比阶段首样本高 15.12 MiB，结束比首样本低 14.50 MiB；可用内存最低 6.68 GiB，压力代理最高 78.87%。System 句柄约 2,523,240 起始，窗口峰值净增 10。分页池约 9.83 GiB，窗口没有正向峰值增长；非分页池约 5.69 GiB，峰值净增 16 KiB。整机 CPU 加权平均 3.83%，磁盘 busy 加权平均 0.97%，读写估算合计约 0.16 MiB/s，页输入峰值 19.42 页/秒。

该运行只说明采样器可用，不构成索引工作负载测试。机器的既有 System 句柄、分页池和非分页池绝对值仍超过 native-host 基线门槛；本轮没有卷访问，也没有用“单次重启可能解决”来豁免这些门槛。

### 追加 metrics-only 基线（2026-11-04）

运行目录：`logs/indexer/current-host-baseline-20261104-02/`。仅运行 20.04 秒 metrics 采样，不访问卷、不枚举文件。23 个样本中 0 个计数器错误。提交量起始 45.23 GiB，窗口峰值增加 2.55 MiB；最低可用物理内存 7.16 GiB，压力比 77.24%–77.35%。System 句柄起始 2,522,818、窗口最大净增 3、结束净减 9。分页池起始 9.17 GiB、峰值增 0.08 MiB；非分页池起始 5.35 GiB、无正向峰值增长。机器 CPU 加权平均 2.57%，采样器进程约 0.07%；整机磁盘读写加权平均约 0.31 MiB/s、busy 1.49%。

此轮背景提交量和压力明显高于前述基线，说明不同时间窗口不能直接当作索引器差值。System 句柄和内核池的异常绝对基线仍触发 native-host 禁止门槛。此观察仅更新背景状态，不放行全卷扫描。

### VHD 上强制 USN/MFT 兼容路径：失败（2026-11-04）

运行目录：`logs/indexer/host-vhd-resume-enumusn-20261104-01/`。为避免 VHD 默认走 FILE_LAYOUT 而绕过 C 卷备用 provider，验证 manifest 新增了仅限本机 `FLOWTARY_TEST` VHD 的 `force_enum_usn`。受控运行确实调用了一次 `FSCTL_ENUM_USN_DATA` 和一次 `FSCTL_GET_NTFS_FILE_RECORD`，随后因 `FILE record update-sequence fixup mismatch` 中止；没有写出 partial checkpoint，也没有发布完整快照。因此初建恢复闭环仍未验证，且备用 provider 已有真实 VHD 失败证据。

该次失败窗口的整机采样：初始提交量约 45.57 GiB，整次运行峰值较起始高 356.51 MiB；最低可用内存 6.16 GiB，压力比最高 80.52%；System 句柄从 2,523,026 起始，峰值未增加，结束少 7；分页池约 9.17 GiB 起始、峰值增 23.12 MiB，非分页池约 5.35 GiB 起始、峰值增 3.11 MiB。真正 provider 阶段只有约 75 ms 和 2 个边界样本，CPU/磁盘无法据此归因，不能称为性能基准；阶段报告计数器错误为 0。VHD 已确认卸载。测试脚本随后加入 fixup 错误诊断字段，但再次启动管理员验收时 Windows 返回“操作已被用户取消”，没有产生第二轮数据。

### USN/MFT fixup 失败的详细诊断（2026-11-04）

运行目录：`logs/indexer/host-vhd-resume-enumusn-20261104-03/`。在可见的管理员 PowerShell 会话中重跑后，精确错误为：`FRN 27; record_bytes=1024; USA_offset=48; USA_count=3; tag=0200; sector tails/replacements=7800:0000,0000:0000`。返回记录既不满足原始 USA tag，也不满足仅允许的“内核已恢复”替换尾值校验；代码按设计失败关闭。失败发生在首次 FILE_RECORD 请求，未生成完整快照或 partial checkpoint，query host 没有可加载的新索引；VHD 已确认卸载。

这轮 6.61 秒整机观测：提交量起始/峰值约 46.53/46.53 GiB（采样精度内净增 0）；最低可用内存 4.75 GiB、压力峰值 84.99%；System 句柄 2,523,143 起始，峰值净增 58，结束 2,523,201；分页池起始 9.21 GiB、峰值增 20.94 MiB，非分页池起始 5.33 GiB、峰值增 17.84 MiB；整机 CPU 样本均值 15.39%，采集进程均值 1.32%；整机读/写速率样本均值约 1.70/9.73 MiB/s。计数器错误为 0。采集阶段约几十毫秒，绝大多数指标窗口含基线与冷却以及宿主其他负载，不能把整窗数字归因于索引器，也不能据此做长期泄漏判断。仅能确认此次短失败没有发布索引，资源测量能力正常采到六类值。

### VHD 的 OpenFileById 替代通道探测（2026-11-04）

运行目录：`logs/indexer/host-vhd-resume-enumusn-20261104-05/`。修正 sequence 0 的 record-zero 探针匹配后，VHD 上枚举了 1 页/144 条记录，解析出 MFT record 0 完整引用 `0x0001000000000000`，`OpenFileById` 打开该 MFT 文件 ID 返回 Win32 error 5（拒绝访问）。因此该备选流读取通道在新建 VHD 上也不可用；不能继续实现 runlist reader 并假设可读取 `$MFT`。同一轮后续完整初建仍在 FRN 27 的混合 USA 尾值处失败，VHD 自动卸载。

该探针独立 14.11 秒六项观测：提交量起始约 46.16 GiB、整窗峰值增 132.52 MiB；最低可用内存 4.68 GiB、压力峰值 85.20%；System 句柄从 2,523,151 起始、峰值净增 13、结束 2,523,120；分页池约 9.30 GiB 起始、峰值增 5.99 MiB，非分页池约 5.34 GiB 起始、峰值增 4.59 MiB；整机 CPU 样本均值 27.19%、探针进程 0.79%；整机读/写样本均值约 0.18/1.64 MiB/s。计数器错误为 0。真正的 File ID open 阶段仅约 100 ms；该窗口同样不能代表持续索引开销，但能证明尝试期间没有句柄持续积累的可见信号。整窗变化包含宿主波动。

随后 30 秒无卷访问冷却观测位于 `logs/indexer/current-host-post-vhd-enum-20261104-01/`。System 句柄从 2,523,071 起始，峰值 +76、结束 +69；分页池峰值 +15.38 MiB，非分页池峰值 +7.92 MiB；提交量峰值 +223.93 MiB；磁盘 busy 均值 22.88%，机器 CPU 均值 13.58%。这些背景变化较大，且采集进程 CPU 均值只有 0.35%，所以不能归因于刚结束的 0.1 秒 probe；目前缺少稳定 idle 控制窗口来证明池/句柄完全回落。计数器错误 0。后续资源验收应采用重复的交错 baseline/workload/cooldown 窗口并保留原始样本，不能以单轮相邻差值作因果结论。

### `FindFirstFileNameW` 硬链接枚举 129 路径样本（2026-11-04）

运行目录：`logs/indexer/host-vhd-hardlink-api-scale-20261104-01/`。VHD 中有 128 个单链接 `.exe` 和 1 个双链接 `.exe`。串行调用 `FindFirstFileNameW/FindNextFileNameW` 129 次，全部成功；双链接样本准确返回 `\workshop\alias-flowtary.exe` 与 `\workshop\Flowtary-1.1.2\flowtary.exe`，128 个单链接样本各返回 1 个名字。provider 累计 59.42 ms，阶段窗口 111 ms；请求数和耗时由 `native_io_stats` 单独报告。阶段边界样本中 System 句柄增量 0、分页池增 24 KiB、非分页池增 16 KiB、提交量变化 -1.98 MiB、最低可用内存 5.36 GiB、机器 CPU 均值 16%、探针进程 3.33%、整机读写速率均值 0。阶段仅两个边界样本，CPU 和瞬时资源值精度不足；129 次成功证明 API 可在 VHD 上列全硬链接，不证明按全卷调用的长期成本。该方法值得继续评估为 Flowtary 自有路径索引的候选，但还需在更大样本、目录路径解析、变更竞争和六项长时资源窗口中验证。随后完整 USN/MFT 初建仍在 FRN 27 fixup 校验失败；没有发布快照，VHD 自动卸载。

结论：小 VHD 的初建、恢复和增量闭环通过；C 卷支持 `FSCTL_ENUM_USN_DATA`，但 `FSCTL_QUERY_FILE_LAYOUT` 的整卷及单文件 ID 调用都返回错误 50。当前不能把完整 FILE_LAYOUT 方案直接用于 C 卷，也不能把单名称枚举冒充完整硬链接索引。正式全盘索引尚未完成。

## 128 MiB NTFS VHD：功能闭环

运行目录：`logs/indexer/host-vhd-20261103-01/`。代码指纹 `0638f1cf410f0d83`，二进制 SHA-256 `2FD05FB97B079AD5544C3F03ECB63C66697C1FCF42FBFF93252FF976DE85BF26`。

脚本 `tools/indexer_host_vhd.ps1` 新建并挂载 128 MiB 动态 VHD；通过实际 image-to-disk 映射、RAW 状态和精确大小检查后，仅格式化该新虚拟盘。原生采集入口还核对卷 GUID、盘符、序列号、NTFS、标签、容量及 File Backed Virtual 总线类型。

- fixture 包含 128 个普通文件、中文名称、两个程序版本和硬链接。仅在新测试盘上显式创建 USN journal；没有修改 C 卷 journal。
- 初次元数据采集发布 164 条索引记录，每个采集进程打开一个卷句柄。
- 重启采集进程并恢复快照，8 次 journal 轮询中 1 次观察到变更；最终 165 条记录。
- 真实创建、删除、改名和新增硬链接后，查询包含创建/改名后的两个 `native` 文件，所有四个可执行名称均可见；原始 `file-000`、已删除 `file-001` 查询为空，中文名称往返正确。
- 最终 `Get-DiskImage` 确认 `Attached: false`。VHD 和快照保留为测试证据，没有删除或改动物理分区。

六类指标按「初建窗口 / 恢复增量窗口」分别为：

1. 整机提交量：起始 38.59 / 38.58 GiB；相对采样峰值增长 119.04 / 109.41 MiB。
2. 内存压力：最低可用物理内存 3.46 / 3.54 GiB。
3. System 句柄：起始 2,522,645 / 2,522,643；峰值相对增长 16 / 18，窗口结束净变化 -2 / -1。
4. 整机磁盘读写：时间加权平均 2.58 / 1.60 MiB/s。
5. 内核池：分页池相对峰值增长 22.98 / 22.61 MiB，非分页池 3.93 / 5.73 MiB；恢复窗口结束非分页池净增长约 0.016 MiB。
6. CPU：整机时间加权平均 5.69% / 4.79%；采集进程按整机容量归一化约 0.18% / 0.22%。进程私有内存采样峰值约 11.29 / 11.05 MiB。

窗口仅约 6.54 / 14.55 秒，包含基线和冷却。计数器错误数与资源保护违规采样数均为 0。整机变化含其他程序活动，不是采集器的因果归因，也不证明长期无泄漏。

## C 卷：兼容性探测

`tools/indexer_host_probe.ps1` 只读取明确指定的 C 卷名称元数据，每次最多 4 页、每页输出缓冲 256 KiB；不建立或发布完整索引、不遍历目录、不修改已有 journal。每轮保留原始计数器、采集报告及实际过滤驱动列表。

- `logs/indexer/host-C-probe-20261103-01/`：`FSCTL_QUERY_FILE_LAYOUT` 整卷首个调用返回 Win32 error 50，立即停止读取并继续观察冷却；没有进入其他扫描路径。
- `logs/indexer/host-C-enum-probe-20261103-01/`：显式选择 `FSCTL_ENUM_USN_DATA`，一页返回 2,433 条文件记录。额外尝试一个实际文件 ID 的 FILE_LAYOUT 也返回 error 50。
- `logs/indexer/host-C-enum-probe-20261103-04pages/`：连续四页成功返回 9,701 条文件记录，未到卷末端。每次只有一个卷句柄，未触发资源保护。两次枚举的代码指纹均为 `88725083c15cc8a8`；所测二进制副本保留在各目录的 `indexer-tested.exe`。

四页窗口约 14.71 秒，六类指标：

1. 整机提交量起始 39.48 GiB，相对采样峰值增长 40.10 MiB。
2. 最低可用物理内存 3.77 GiB；Pages Input/sec 采样峰值约 31,166。阶段边界的短间隔可放大瞬时速率，不能将其当作持续换页速率。
3. System 起始 2,522,511 个句柄；采样峰值相对增长为 0，结束净变化 -27。
4. 整机磁盘读写平均约 1.01 MiB/s，短间隔采样峰值约 121.74 MiB/s；不等于采集器独占吞吐。
5. 分页池峰值增长约 0.12 MiB，非分页池峰值增长约 0.32 MiB；非分页池窗口结束净变化约 -3.62 MiB。
6. 整机 CPU 平均约 3.39%，探测进程约 0.10%；探测进程私有内存峰值约 10.82 MiB。

第一轮单页窗口仍观察到 System 净增 19、非分页池净增约 6.82 MiB；四页窗口没有重复该增长模式。样本不足以归因，也不能把任何一轮包装成零泄漏证明。原始数据和计算摘要分别在各目录 `samples.jsonl`、`summary.json` 中。

C 卷的实际过滤实例列表包含 wcifs Outer Instance，也包含多种其他过滤器。仅凭 error 50 和过滤器存在不能判定是谁拦截了调用，更不能判定其是旧内核泄漏的分配者。

## 实际 C 卷 FILE 记录解析

`logs/indexer/host-C-mft-record-20261104-01/` 在一页枚举后，仅对其中一个明确文件 ID 调用 `FSCTL_GET_NTFS_FILE_RECORD`；接口与名称解码均成功，保留一个卷句柄，资源保护未触发。记录来自已恢复 update-sequence 的内核缓存格式，所有扇区尾部均通过对应 USA replacement 检查。所测代码指纹为 `f1e169b5417fd762`，二进制副本保留在运行目录。

该记录头的 hardlink 字段为 2，当前记录中解出的非 DOS 名称为 1；这不能直接推断丢失了一个扩展名称，需要区分 DOS 命名属性计数和扩展记录。解析器后续已增加原始 FILE_NAME/DOS 属性数量与 attribute-list 标记，供批量组装时校验。单条记录解码成功仍不代表全卷批量通道或全部名称已完成。

## MFT 批量读取兼容性探针

`logs/indexer/host-C-raw-mft-zero-20261104-01/` 在一页 USN 枚举之外请求一次 `FSCTL_GET_NTFS_VOLUME_DATA`，并尝试从卷设备读取 MFT 的第 0 条记录。几何返回成功，但卷设备 `ReadFile` 返回 Win32 error 50。探针随即停止，没有重试或扩大读取范围。

`logs/indexer/host-C-mft-stream-zero-20261104-01/` 与 `logs/indexer/host-C-mft-stream-backup-20261104-01/` 改为通过 `\\?\C:\$MFT` 读取首条记录；两次均在打开受保护的 `$MFT` 流时返回 Win32 error 5（拒绝访问），管理员令牌下显式尝试备份读取权限也未能打开。每次都在单条读取前结束。由此确认当前主机上两种直接读取通道均不可用；**没有据此启用逐记录 ioctl 或任何全卷扫描**。

随后开发了只在首个 FILE_LAYOUT 调用返回 50 时启用的 USN+FILE_RECORD 初建兼容路径。路径最多处理 1,000,000 条记录、总计 120 秒，并在完成前校验 base/extension 的全部 FILE_NAME 属性数量（含不索引的 DOS 别名）与 NTFS hard-link header count 完全一致，否则拒绝发布。离线组装与 add-only 多链接测试通过，但目前**尚未得到真实卷运行证据**。`logs/indexer/host-C-mft-fragment-batch64-20261104-01/` 的受控 64 条记录测量在打开卷前因可用内存仅约 0.93 GiB（低于 1.5 GiB 门槛）而拒绝执行；该轮没有发出 MFT 记录查询，也没有打开卷句柄。

三个探针只观察到一次卷句柄；各次冷却窗口的句柄和内核池变化不一致，且整机内存提交、磁盘活动存在较大背景波动。短观察不能用于推断稳定性或泄漏归因。后续应先寻找 Windows 支持、可批量返回完整 FILE_NAME/硬链接元数据的接口，或设计经过证明有严格总工作量上限的其他通道；在解决前，C: 不能进行完整索引，也不能假报 hardlink 名称完整。

## 实现上的后续工作

保留 GoZ 的解析/FRN 图复用，但按卷选择元数据能力。当前 C 卷可用的枚举接口不能单独保证完整硬链接名称，需要增加完整 MFT 文件名属性解析通道，或找到另一个经过验证的批量元数据通道。不能逐文件补开路径、不能回退 fd，也不能静默丢弃别名。

已引入纯解析模块 `omerbenamram/mft`，发布版本 0.9.2、VCS 提交 `33c8730fd21c68e2ceaafa30d0effd0735396976`（MIT/Apache-2.0），关闭默认 CLI 功能。新增有界 FILE 记录名称解析、fixup 校验、UTF-16/WTF-8 保留及 base/extension 归属测试；来源与校验和见 `indexer/MFT_PARSER.md`。尚未完成卷上 MFT 数据区段的批量读取与扩展名称合并。解析器通过测试不等于已解决在线一致性、稀疏/分段 runlist 和完整全盘索引。

接下来还需完成真实全卷初建与持续增量、持久化/IPC/正式 UI 接入，并覆盖更长空闲及批量变更、journal 断档、恢复和关闭的资源行为。上述小样本均不能替代这项验收。

## Resident `$ATTRIBUTE_LIST` 扩展记录核验更新

`logs/indexer/host-C-mft-fragment-page5-even128-20261104-01/report.json` 的均匀 128 条样本中，发现 2 个 base 记录的 hardlink header 为 2、当前 FILE_NAME 属性数为 1，且包含 `$ATTRIBUTE_LIST`。FRN 分别为 `53198770598324206` 与 `13792273858834284`。此前仅按 DOS 别名解释差异不成立；完整性审计必须读取引用的扩展记录。

解析器现解析 resident `$ATTRIBUTE_LIST` 中 FILE_NAME 属性的扩展 FRN/序列号，初建时读取并合并扩展名称，增量硬链接刷新也做同样合并；非驻留 attribute-list 明确失败。另处理了初建提前读取扩展记录后，USN 顺序再次枚举到它时重复计数的问题。离线 21 项测试和 release 编译通过，但尚无这两个真实扩展记录的读取结果，因而此兼容路径仍未通过真实卷完整性验收。

### 原生采集调用扇出观测

由于 C 卷 fallback 当前对每条枚举记录调用一次 `FSCTL_GET_NTFS_FILE_RECORD`，并可能对 `$ATTRIBUTE_LIST` 扩展记录追加调用，整机六项指标之外还必须记录 provider 操作数与耗时。`collect` 现在在顶层结果输出 `native_io_stats`：FILE 记录 ioctl 数/耗时、USN 枚举页数/耗时、journal 读取数/耗时、FILE_LAYOUT 请求数/耗时；统计通过共享快照保留，因此采集取消或错误时也可输出已累计值。源码改动后 release 编译通过；没有为检查此字段运行物理卷采集。这个计数用于量化调用扇出，不等于内核池/句柄归因，也不能代替六项整机测量。当前 Windows 文档接口不提供多 FRN 批量 `FSCTL_GET_NTFS_FILE_RECORD`：C 卷 FILE_LAYOUT 返回 error 50，卷设备读 `$MFT` 返回 error 50，受保护 `$MFT` 流打开返回 error 5。若这些限制仍成立，必须另行实现经验证的批量 MFT 区段读取机制；不能靠并行或扩大逐记录 ioctl 上限补偿。

为取得最小 live 证据，`probe-volume` 增加 `--probe-frns`，最多 4 个显式 FRN、每个最多 8 个扩展引用；`tools/indexer_host_probe.ps1` 增加对应 `-ProbeFrns` 参数。2026-11-04 的 5 秒 metrics-only 基线在 `logs/indexer/host-C-targeted-frn-attrlist-20261104-01/baseline-report.json`：整机提交量约 40.57 GiB / 72.74 GiB（55.8%），最低可用物理内存 9.78 GiB，System 句柄 2,522,355，分页池约 9.19 GiB、非分页池约 5.30 GiB，整机 CPU 平均约 4.36%，磁盘忙碌平均约 1.36%，无计数器错误。可用内存和提交率符合本探针的动态门槛，但仍存在异常的既有句柄/内核池基线。实际探针因当前 PowerShell 非管理员令牌而被 Windows `#requires -RunAsAdministrator` 拒绝，未启动子进程、未打开卷；不应把拒绝记成一次卷测试。

### 文件 ID 打开 `$MFT` 的一次性兼容探针（代码已编译，实卷待测）

旧探测确认卷设备原始读返回 error 50、直接打开 `\\?\C:\$MFT` 返回 error 5。现新增显式 `--probe-mft-by-file-id`：先用一次 `FSCTL_GET_NTFS_FILE_RECORD` 解析 FRN 0 的完整引用，再通过 `OpenFileById` 尝试只读打开该 MFT 文件 ID，只读取首个最多 64 KiB 数据块；只有记录头自报大小有效、原始 USA fixup 严格通过且 record 0 能解析，才报告流读取通道可用。它不会进入 `collect`，不会重试，也不会扩大到整卷。源码已通过 release 编译，尚未在卷上执行。复现入口为 `tools/indexer_host_probe.ps1 -RunDirectory <新的 logs/indexer 目录> -ProbeMftByFileId -Mechanism enum-usn`；该脚本要求管理员会话并使用既有 host-probe manifest 门槛。若此入口成功，下一步仍需在仅新建的回滚 VHD 上实现和验证 bounded runlist reader 与 MFT/USN 一致性；一次记录读成功不等于全盘可索引。

### IOCTL payload 偏移复核（2026-11-04）

对 `FSCTL_GET_NTFS_FILE_RECORD` 解包再次核对：输出先含 8 字节文件引用和 4 字节记录长度，解析器仅把后续声明长度的 payload 交给 FILE/USA 校验。`host-vhd-hardlink-api-scale-20261104-01/initial-interrupted-report.json` 记录了 1 次文件记录请求、1 页 USN 请求；错误诊断来自 1,024 字节 payload（USA offset 48/count 3，tag `0200`，尾值/替换值 `7800:0000,0000:0000`）。当前证据不支持“输出头偏移错 12 字节”的解释。仍需用新的受控 VHD 独立交叉检查原始返回字节；不得通过放宽 fixup 校验继续初建。

The diagnostic build now supports `probe-volume --host-test-vhd --probe-frns <one-FRN> --capture-file-record-payload-on-error`. It appends at most 4 KiB of the raw FILE-record payload to the failure text, and refuses the option outside a verified test VHD or when more than one FRN is requested. The host VHD script exposes this as `-CaptureFileRecordPayload` alongside `-VerifyInitialResume`. Release compilation and the existing 22 Rust tests pass. The first raw-diagnostic host-script attempt exited before the planned fixture sequence completed; a corrected script in the next fresh run captured the payload and continued through hard-link measurement and the initial-build failure.

### 原始 FILE_RECORD payload 复核与回归运行（2026-11-04）

运行目录：`logs/indexer/host-vhd-raw-payload-20261104-02/`。新增的 VHD-only 单 FRN 诊断在新建测试盘上捕获完整 1,024 字节 payload；离线解码字段为 `FILE`、USA offset 48、count 3、tag `0x0002`、sector tail/replacement `0x0078/0x0000` 与 `0x0000/0x0000`。因此 FRN 27 的记录确实未通过 USA 校验，不能作为格式正确的 FILE 记录发布。VHD 结束时确认卸载。

同一运行的 `FindFirstFileNameW/FindNextFileNameW` 串行验证覆盖 129 条路径：1 个双链接和 128 个单链接均返回预期名称。累计 provider 时间 39.706 ms，129 次请求。阶段窗口 56.43 ms、仅 2 个采样点：整机提交量边界变化 -2.48 MiB，可用内存最低 7.16 GiB、压力比最高 77.35%，System 句柄阶段净变化 0，分页池 +0、非分页池 +12 KiB，整机 CPU 均值 18.97%、探针进程 5.17%，整机磁盘读写样本均值 0、磁盘 busy 18.94%；计数器错误 0。如此短的窗口和整机背景活动不能归因到 API，也不能外推全盘成本。

初建阶段仍只调用 1 页 USN 和 1 次 FILE_RECORD 请求（68 µs），在 FRN 27 失败；没有写入 checkpoint 或发布 snapshot。该次边界窗口 89.9 ms、2 个样本，System 句柄净变化 0、分页池约 +96 KiB、非分页池约 +536 KiB；提交变化约 -1.25 MiB、最低可用内存 7.16 GiB、压力比最高 77.33%、CPU 均值 4.39%、磁盘读写样本均值 0。计数器错误 0。因阶段不到 0.1 秒，这些资源变化仅是监测读数，不是可用的泄漏/成本估计。

测试脚本现把“probe 进程按预期以解析失败退出、但原始 payload 已完整保存”记录为诊断成功，并继续执行硬链接 API 测量和初建尝试。回滚 VHD、原始 JSONL 和报告均保留在运行目录。


### 双次记录读取与 USN 硬链接枚举能力验证（2026-11-04）

运行目录：`logs/indexer/host-vhd-raw-payload-20261104-04/`。同一新建 VHD 上对 FRN 27 分两次、两个独立 probe 进程读取：各 1 次 `FSCTL_GET_NTFS_FILE_RECORD`，耗时 92/86 µs，1024 字节 payload 完全一致，USA mismatch 字段一致；这排除了本次样本是单次瞬态读数的解释。它不能证明其他文件记录都同样异常。VHD 最终确认卸载。

新增的单页 USN 名称重复统计返回 273 条记录、0 个重复 FRN；但同一页包含已知双硬链接文件。与 129 次 `FindFirstFileNameW` 正确返回两个名字的结果一起看，当前 `FSCTL_ENUM_USN_DATA` 页不足以证明完整硬链接路径。候选兼容路线因此需要从 USN 建立父目录/文件名图，再为文件调用路径硬链接 API；这条路线的路径解析正确性和全卷调用成本尚未实现/验证，不能现在启用。

129 路径硬链接 API 阶段共 33.879 ms provider 时间，阶段窗口 54.46 ms、2 个采样点：提交量边界 +40 KiB，可用内存最低约 5.02 GiB、压力比最高 84.13%，System 句柄变化 0，分页池 +124 KiB、非分页池 -732 KiB；整机 CPU 均值 7.46%、探针进程 4.48%、磁盘 busy 1.32%、整机读写约 1.18 MiB/s。另一轮单记录诊断窗口 84.8 ms、2 点：提交变化 -1.74 MiB、可用内存最低 4.89 GiB、System 句柄变化 0、分页/非分页池 +8 KiB/+544 KiB、整机 CPU 4.13%、磁盘 busy 1.82%、读写约 0.047 MiB/s；计数器错误均为 0。这些短阶段边界样本包含宿主活动，不能据此推算长时扫描成本。

同轮完整初建仍在 FRN 27 fail-closed，只有一页 USN 和一次 FILE_RECORD 请求；未产生 checkpoint 或 snapshot。六项系统计数器在初建阶段均有值，但该阶段不足 0.1 秒，不是初建开销验收。

### 重复 USN 名称统计的回归结果（2026-11-04）

运行目录：`logs/indexer/host-vhd-raw-payload-20261104-04/`。对 FRN 27 的两次独立读取均返回相同 payload（SHA/逐字节比较相等；每次一个 volume handle、一个 FILE_RECORD 请求，92/86 µs），VHD 已自动卸载。USN 单页 273 条记录中检测到 0 个重复 FRN；单页同时未到卷末端。该实验无法用 USN 输出自身列出 fixture 双链接的两个名字，因此完整名称仍需另一来源。

该轮 `FindFirstFileNameW/FindNextFileNameW` 129 次调用全部成功、累计 33.879 ms，双链接结果正确。阶段采样 2 点、54.46 ms：提交边界 +40 KiB、最低可用内存 5.02 GiB、压力 84.13%、System 句柄净变化 0、分页池 +124 KiB、非分页池 -732 KiB、CPU 7.46%（探针进程 4.48%）、磁盘 busy 1.32%、读写 1.18 MiB/s、计数器错误 0。该小样本只适合做 API 候选吞吐参考，不能线性外推为正式全卷性能或内核资源稳定性证明。

### USN 终止边界与宿主测试二进制一致性（2026-11-04）

USN provider 的根目录由私有索引图预建；`FSCTL_ENUM_USN_DATA` 到达 EOF 时只要求确认已消费过至少一页，不要求枚举记录再次出现根 FRN。普通记录的父 FRN 仍须在目录图中可解析，路径硬链接 reconciliation 仍须返回输入路径并通过父目录校验。代码移除了未参与正确性判断的 `saw_root` 状态。`cargo check --release --locked`、`cargo build --release --locked` 和 `cargo test --release --locked` 均通过（25 项）。

进一步收紧了 NTFS 元数据调用面：正常索引不再通过 `FSCTL_GET_NTFS_FILE_RECORD` 为每个 FRN 单独补读 FILE record。初建以 USN 名称页构建目录图，硬链接集合使用串行 `FindFirstFileNameW/FindNextFileNameW`；`QUERY_FILE_LAYOUT` 对增量请求返回 error 50 时，也切换到已有目录图上的路径硬链接 API。单记录 FILE ioctl 只保留给显式、限量的诊断入口。Microsoft 文档说明 FILE_RECORD ioctl 每次仅返回一个在用记录，且可能返回请求 FRN 之前的另一条记录；这进一步确认它不是全卷批量元数据通道：[FSCTL_GET_NTFS_FILE_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_ntfs_file_record)。源码 release 编译和 workspace 测试通过（270 项单测/集成测试/文档测试）；PowerShell VHD 脚本解析通过。此路径尚未在新 VHD 上通过完整闭环。

### 当前 USN 初建失败与整机资源窗口（2026-11-04）

运行目录：`logs/indexer/host-vhd-resume-enumusn-20261104-11/`。脚本按当前源码重建 release 二进制、创建并验证新的 128 MiB `FLOWTARY_TEST` VHD，成功写出 `test.partial.fti`，然后从 durable checkpoint 恢复；VHD 最终已卸载。恢复在发布前失败，原因是 FRN 32 `$Tops` 的父项 `$TxfLog` 位于 NTFS 内部不可达链（`InLostFound`）。断点前后各请求 1 页 USN；FILE_RECORD、FILE_LAYOUT、路径硬链接 API 请求数均为 0，说明 USA 不匹配已不再阻断这条 USN 路线，当前阻断点是内部元数据记录的路径分类。

该轮约 6.5 秒机器窗口记录了六项系统指标且计数器错误为 0：提交量峰值约 53.16 GB（约 49.5 GiB，提交比例最高 68.05%）；可用内存最低约 3.94 GiB、压力比最高 87.53%；System 句柄峰值约 2,523,573（宿主既有基线约 2.52M）；分页池/非分页池基线约 9.4/5.4 GiB；CPU 阶段均值最高约 34.2%；磁盘阶段平均 busy 最高约 20.2%，最高吞吐阶段约 28.1 MB/s。初建阶段约 14.8 ms、快照恢复阶段约 56 ms，这些短窗口不能归因给 indexer，也不是稳态扫描评测。此前一次启动在卷打开前被资源守卫取消：页输入峰值约 197,529 页/秒，native volume open attempts 为 0。完整功能与整机资源验收仍未通过。

代码现仅把“名字和父项都以 `$` 开头，且两者路径均落在 `InLostFound`”的链标记为不可达 NTFS 元数据并跳过硬链接 API；普通路径不完整项继续 fail-closed。该新分类已通过离线 workspace 测试，但尚无新的 VHD 运行结果。测试脚本一次运行已覆盖构建、VHD 探针、checkpoint/resume、USN 变更和冷却采样；之后应由一个管理员进程跑完整套件，避免为各阶段分别触发 UAC。

VHD harness 现于创建/格式化测试盘前检查 5 秒整机基线：必须覆盖六项指标、至少 5 个样本且无计数器错误；提交比例不高于 85%、可用 RAM 至少 1.5 GiB、分页输入峰值不高于 10,000 页/秒、CPU 均值不高于 85%、磁盘 busy 均值不高于 90%。已存在的 System 句柄与池异常作为 warning 记录，和资源增长守卫分开处理。该门槛由脚本解析检查通过，尚未通过新的管理员 VHD 运行验证。

先前 VHD 报告中的 FILE_RECORD 请求与当前强制 USN provider 源码预期不符；宿主脚本原先直接复用已有 `target/release/flowtary-indexer.exe`，未重建或校验其来源。现已改为创建 VHD 前按当前工作树执行 `cargo build --release --locked`，并把执行二进制 SHA-256 记入完成状态，避免复用旧二进制。当前 shell 非管理员，尚未重跑 VHD，因此该修正尚无新的 NTFS 运行证据；此前 USA 不匹配仍是初建的未解决失败。旧报告的六项系统计数器在约 71 ms 初建窗口有记录，但不足以用于扫描资源验收。

### 新的 metrics-only 基线与 VHD 启动状态（2026-11-04）

`logs/indexer/host-baseline-20261104-181813/` 是一次 15 秒、无需管理员权限、无卷访问的宿主采样：提交量峰值约 51.01 GB（十进制，约 47.5 GiB；比例 65.31%），压力峰值 79.81%，可用物理内存最低 6.38 GiB；System 句柄约 2,523,328，分页池约 9.35 GiB、非分页池约 5.42 GiB；整机 CPU 均值 4.29%，磁盘 busy 均值 2.13%，均值吞吐约 0.68 MB/s，页输入峰值约 1,139 页/秒；16 个机器样本、计数器错误 0。15 秒内 System 句柄净变化 -10，分页池增加约 3.0 MiB、非分页池增加约 0.42 MiB。该样本说明当前宿主资源守卫会通过内存/提交/CPU/磁盘条件，但高句柄与内核池是预先存在的异常基线；metrics-only 进程的约 10 MiB 私有提交不能代表索引器成本。

随后只发起了一次 `RunAs` 完整 VHD harness（`host-vhd-complete-20261104-181542`）。Windows 返回“操作已被用户取消”；目录未创建，未生成 harness 状态文件，也没有 flowtary-indexer 或 DiskPart 子进程。该尝试不算 VHD 测试或卷访问。为避免重复 UAC，本轮没有重试。Rust workspace 仍为 270 项测试通过，release 构建成功，PowerShell harness 解析通过；新增 `$Tops/$TxfLog` 分类仍需新的 VHD 闭环验证。

离线复核发现，原来的“任意 `$` 名称位于任意 `$` 父目录且两者都孤立”豁免范围过宽，可能把用户创建的 `$` 命名孤儿项也略过。现已收紧为只豁免精确的 `$Tops` → `$TxfLog` 且两者路径均在 lost+found 的关系，并为 `$user-data` 和不同父目录的 `$Tops` 加反例断言。workspace 全部 270 项测试通过，release 构建通过。此收紧降低误跳过路径的风险，但仍需要下一次 VHD 初建/恢复闭环确认真实关系及增量行为。

### C++ 集成重建、测试与部署（2026-11-04）

使用已启用 `FLOWTARY_EXPERIMENTAL_INDEXER=ON` 的 Release 配置重建并部署到 `C:\workshop\Flowtary-1.1.27.10`。应用与 `flowtary-indexer.exe` 的部署 SHA-256 均与相应 build 产物一致；7 项 CTest（应用目录、索引搜索/UI、IPC 及既有测试）全部通过。该部署只包含查询 helper；当前没有已验收的生产卷快照，所以不能证明全盘收集已上线。

新增整机内存压力硬门槛为 90%：运行时 resource guard、VHD 测试前预检和 C++/Rust IPC 基准预检均会拒绝超限状态；C++/Rust IPC 预检还限制 5 秒内 paging input 峰值不超过 10,000 页/秒。`logs/indexer/cpp-ipc-steady-60s-5qps-pid-20261104-01/` 的 60 秒查询样本在压力 93.70%、available 1.99 GiB、页输入峰值 136,752 页/秒时运行；故保留为观测样本并明确标为宿主过载污染，不能当作 Flowtary 的整机成本或通过门槛。该结果促使新增门槛，后续重测须先通过预检。

预检上线后的重测目录为 `logs/indexer/host-baseline-gated-20261104-183939/` 和 `logs/indexer/cpp-ipc-steady-60s-5qps-gated-20261104-01/`。8 秒基线和后续 5 秒基准预检都通过，异常的 System 句柄/分页池/非分页池基线只记录警告。60 秒查询期间系统提交量峰值 +12.3 MiB、压力最高 86.37%、可用内存最低约 4.31 GiB、System 句柄峰值 +128、分页/非分页池峰值 +1.96/+8.90 MiB、CPU 均值 8.49%、磁盘 busy 2.48%、页输入峰值 4,899 页/秒，计数器错误为 0。该查询评测覆盖六项指标但不含真实卷访问，系统级变化不能归因给 Flowtary。

### Flowtary 自有 `C:\workshop\*.exe` 路径 API 资源探针（2026-11-04）

新增 `tools/workshop_link_resource_probe.ps1`，只读地在 `C:\workshop` 下限时/限深遍历，跳过 reparse point，最多收集 128 个 `.exe`、4,096 个目录、深度 12；随后最多执行 8,192 次 `FindFirstFileNameW/FindNextFileNameW`，每个句柄由 `SafeHandle` 的 `FindClose` 释放。启动前要求整机采样通过提交量、压力、可用内存、paging input、CPU、磁盘门槛；循环每 8 轮读取系统计数作即时停止检查。

权威复测报告：`logs/indexer/workshop-link-probe-20261104-190200/report.json`，原始六项整机样本在同目录 `machine.jsonl`。预检通过，压力峰值 78.37%、可用内存最低约 6.84 GiB、提交比例峰值 67.21%、页输入峰值约 19.6 页/秒；计数器无错误。遍历 49 个目录找到 56 个 `.exe`，用时 60 ms、无遍历错误。随后串行执行 8,176 次文件路径调用（146 轮），返回 8,176 个硬链接名称、0 个 API 错误，用时 3.382 秒（约 2,418 调用/秒）。API 阶段 4 个整机样本：提交量相位峰值增长 0、压力峰值 78.47%、可用内存最低约 6.80 GiB、System 进程句柄峰值 +1、分页池峰值增长 0、非分页池峰值增长约 2.16 MiB、整机 CPU 加权均值 13.02%、磁盘 busy 3.98%、读写合计约 0.50 MiB/s、页输入峰值约 103 页/秒。直接 System 进程句柄边界样本为 2,523,474 → 峰值 2,523,474 → 结束 2,523,472；探针 PowerShell 进程句柄保持 747，私有字节前后相同。冷却 9 个机器样本中 System 句柄峰值未增加；分页池/非分页池冷却段各有约 0.68/3.36 MiB 的机器级变化，仍混有背景活动。

另行明确句柄口径：边界快照通过 `GetPerformanceInfo.HandleCount` 取得的是**所有进程句柄总量**（本次约 522.6 万），Rust 整机 sampler 的 `system_handles` 则是 **`\Process(System)\Handle Count`**（本次约 252.3 万）。两者不可混称。此探针只验证真实 `C:\workshop` `.exe` 文件上的路径硬链接 API，未读文件内容、未打开原始卷、未做全盘初建；系统级短时变化不能单独归因给探针，也不能据此排除全量扫描时的 Filter Manager 资源增长。

### 最新无卷访问六指标窗口与单次提权启动器（2026-11-04）

`logs/indexer/host-baseline-current-20261104-02/` 是 10 秒 metrics-only 样本，原始样本为 `samples.jsonl`，Rust 汇总为 `report.json`。计数器错误样本为 0。窗口整机提交量峰值 52.56 GB（提交比例峰值 67.30%）；内存压力峰值 80.59%，可用物理内存最低 6.14 GiB；System 句柄基线 2,523,322、峰值 2,523,330、窗口末值 2,523,306；分页池峰值 10.12 GB、非分页池峰值 5.87 GB；整机 CPU 加权均值 18.51%；磁盘 busy 加权均值 1.92%，平均吞吐约 1.10 MiB/s，页输入峰值约 116 页/秒。此窗口没有运行卷探针，背景机器活动使 CPU 与磁盘数据不能归因给 Flowtary；采样进程 private bytes 峰值约 9.60 MiB。

`tools/indexer_host_validation_once.ps1` 将完整隔离 VHD harness 放在单个提升权限的 PowerShell 进程中，避免每阶段单独 RunAs。2026-11-04 的一次启动返回退出码 1，且旧版启动器没有留下目录或状态文件，故无法从该尝试判定失败发生在 harness 哪一阶段，也没有新的 VHD 结果。启动器已改为通过自身 elevated-child 模式执行 harness，并在子进程外层写启动/退出状态；该修正版尚未再次触发 UAC。全盘索引闭环仍待验证。

2026-11-04 19:34 的修正版单次启动器也返回 1；启动日志记录了 elevation 子进程退出码，但测试目录/state.json 未生成，表明 VHD harness 尚未进入可留存状态的阶段。调用 shell 的 token 为 Medium Mandatory，Administrators SID 是 deny-only；这并不能证明 elevated child 的 token 状态。启动器现会在子进程最早阶段记录身份、管理员角色判断和 RunDirectory，以便后续单次运行准确区分“未提升”和参数/脚本启动失败。本轮不再发起新的 UAC；没有创建 VHD，也没有打开生产卷。当前 metrics-only 仍是唯一最新整机数据。

最新 Rust 定向回归（无原始卷访问）：`cargo test --locked --manifest-path indexer/Cargo.toml -p goz-core only_unreachable_dollar_metadata` 通过 1 项；`guard_checks_system_resources_and_missing_counters` 与 `native_host_gate_rejects_memory_pressure_above_ninety_percent` 各通过 1 项。它们分别证明 `$Tops/$TxfLog` 豁免边界、六项资源增长守卫及 90% 压力门槛的单元行为，不能替代新的 VHD 初建/恢复证据。

单次启动器排错结论补记：`host-validation-20261104-195335-child.json` 表明该次 elevated child 已进入启动器，但 PowerShell 将 VHD harness 的字符串参数数组按位置绑定，失败于 `RunDirectory` 参数。启动器已改为 hashtable 命名参数 splatting，并用 PowerShell 7 本地参数绑定自检通过。随后 `host-validation-20261104-195458` 的 UAC 被取消；该次没有 child token 文件、harness 目录、VHD 创建或卷打开。启动器现在把 UAC 启动失败单独记入 `*-result.json`，已完成脚本解析验证；没有再次请求 UAC。

现有百万条离线基准为浅层结构：1,000,000 个文件基本都位于单一 `data` 目录，未模拟大目录树或硬链接比例。因此它证明的是记录数规模，不覆盖真实文件系统的目录层级/硬链接路径成本。下一步离线补充更贴近卷目录结构的可复现实验，并保留真实 NTFS 闭环为单独验收项。

### 归因采样器复核（2026-11-04）

`logs/indexer/host-attribution-current-20261104-01/` 是 5 秒 metrics-only 归因采样；该窗口没有访问卷或执行索引。机器样本覆盖 5.02 秒、6 个观测点，计数器错误为 0。提交量净增约 13.99 MiB，内存压力从约 92.17% 到 92.22%（峰值约 92.22%），可用物理内存最低约 2.46 GiB；System 句柄由 2,523,655 到 2,523,652，窗口峰值 2,523,660；分页池净增约 2.24 MiB，非分页池净减约 0.75 MiB；整机 CPU 加权均值约 12.72%，磁盘 busy 加权均值约 2.53%，页输入峰值约 87.6 页/秒。由于压力已超过 90% 门槛，这只是过载宿主上的观察样本，不得作为索引负载验收或归因证据。

同时采集的进程快照显示 PowerShell 采样驱动进程 private bytes 净增约 23.95 MiB；metrics-only 的 `indexer-tested.exe` 进程 private bytes 净增约 8.65 MiB、句柄净增 153。该可执行文件只运行机器计数器采集，没有枚举卷；它的 private bytes 和句柄变化反映采集器本身/运行时分配，不代表生产索引器做目录扫描时的成本。此归因层可用于发现同窗口内的进程级变化，但不能把机器池、System 句柄、提交量等整机差值自动归因给某个进程。

### 最新 VHD 运行前拒绝基线（2026-11-04）

`logs/indexer/host-preflight-20261104-01/` 是一次 8 秒 metrics-only 预检，0 卷打开、0 目录枚举、9 个样本、计数器错误 0。提交率峰值 76.55%，但内存压力峰值 94.13%、最低可用内存 1.85 GiB；页输入峰值 146/秒，CPU 均值 12.59%，磁盘 busy 均值 2.15%。System 句柄约 2,523,631，分页池约 10.15 GiB、非分页池约 5.49 GiB。压力高于 VHD harness 的 90% 拒绝线，因此没有发起 UAC、没有创建 VHD 或访问卷。此样本不作为索引负载成本结果，只记录提权前的宿主门槛判定。

### 后续只读预检与压力趋势（2026-11-04 20:33）

`logs/indexer/host-preflight-20261104-203317/` 是紧接前一拒绝窗口后的 8 秒 metrics-only 采样：9 个样本，计数器错误 0。提交率峰值 76.88%，内存压力 94.81%，可用内存最低 1.64 GiB；System 句柄 2,523,615，分页池约 10.24 GiB、非分页池约 5.50 GiB；CPU 均值 12.22%，磁盘 busy 均值 2.26%，页输入峰值 1,086/秒。相较 20:31 的预检，压力升高约 0.68 个百分点、可用内存再降约 0.21 GiB；没有运行索引或卷探针。该窗口继续不满足 90% 压力及 2 GiB 可用内存门槛，因此仍不启动 UAC/VHD harness。

### UAC 前置资源门控（2026-11-04）

`tools/indexer_host_validation_once.ps1` 现在先以普通权限运行 5 秒 metrics-only 六指标预检；只有提交率 ≤85%、压力 ≤90%、可用内存 ≥1.5 GiB、提交量增长 ≤1 GiB、页输入 ≤10,000/秒、CPU 均值 ≤85%、磁盘 busy 均值 ≤90%、System 句柄增量 ≤512、分页池增长 ≤256 MiB、非分页池增长 ≤128 MiB、计数器完整时，才启动唯一一次 RunAs UAC。提交、句柄和内核池增量以预检首个基线样本为参照，检查窗口内峰值。提升后的 VHD harness 仍会再次预检，避免提权等待期间状态恶化。拒绝时结果与原始样本写入运行目录旁的 `*-preflight-result.json` 和 `*-preflight/`，不创建 harness 目录、不触发 UAC。本次只完成脚本解析验证，尚未再次执行预检或触发 UAC。

实现后实际调用 `host-validation-single-uac-20261104-01` 验证早拒绝路径：5 秒窗口 6 个机器样本，计数器错误 0；提交率峰值 77.22%，压力峰值 94.90%，可用内存最低 1.61 GiB，页输入峰值 934/秒，CPU 均值 11.33%，磁盘 busy 均值 1.27%；System 句柄峰值增 4，分页池峰值增 4.88 MiB、非分页池增 0.23 MiB。由于压力超线，启动器记录 `rejected-before-uac`，没有弹 UAC、没有创建 VHD 或打开卷。PowerShell 解析通过。该改动避免在当前宿主压力下浪费提权确认；不会替代真实 NTFS VHD 验证。

### 硬链接补全阶段的规模瓶颈

当前 USN 兼容路径在枚举后，对每个普通文件 FRN 单独调用一次 `FindFirstFileNameW/FindNextFileNameW`，并在每 128 个 FRN 后检查点；单次初建硬链接补全限时 120 秒、请求上限 1,000,000。`workshop-link-probe-20261104-190200/report.json` 的 8,176 次调用在 3.382 秒完成，约 2,417.5 次/秒；此探针是在 56 个现有 `.exe` 路径上重复调用，属于暖路径微基准，不代表百万个不同路径的性能。

仅作容量风险估算：按该暖路径速率，10 万、100 万、800 万文件分别约需 0.69、6.89、55.15 分钟；120 秒预算内约能完成 290,100 次调用。由于初建阶段近似每文件一次调用，当前机制无法在 120 秒内完整补齐 100 万文件的链接关系。不能通过提高并发或放宽资源守卫处理这个差距；在宣称全盘可用前，需要以真实 VHD 测量目录/文件 ID 批量枚举或经验证的批量 MFT 读取路径，并同时通过六项整机遥测和完整性校验。

### 批量目录枚举候选 API（待 VHD 验证）

当前 `windows-sys 0.61.2` 已包含 `GetFileInformationByHandleEx`、`FileIdBothDirectoryInfo` 和 `FILE_ID_BOTH_DIR_INFO` 绑定。该接口返回目录缓冲区中的多条名称记录，每条带 64 位 `FileId`、属性、大小/时间和 UTF-16 名称；按 FileId 聚合可望在一次目录批次里同时发现多个硬链接路径，避免逐文件执行 `FindFirstFileNameW`。这是待验证的替代方案，暂未接入生产 provider。

后续 VHD 探针必须先证明：NTFS FileId 与完整 FRN/序列号的映射；批次游标在目录变化后的无遗漏行为；reparse point 不会被跟随；同一文件的多个父目录路径能合并；取消和错误时目录句柄确定关闭。再通过 journal replay 和 checkpoint/resume 校验后，才能比较批次数、耗时与六项系统指标并决定是否替换当前逐文件补链流程。不得仅凭 API 声明或目录吞吐微基准放宽当前 120 秒期限。

### VHD 批量目录 API 探针实现记录（初版状态）

增加了仅限授权测试 VHD 的 `--probe-directory-batch RELATIVE_DIR`。它用 `GetFileInformationByHandleEx(FileIdBothDirectoryRestartInfo/FileIdBothDirectoryInfo)` 按 64-KiB 缓冲区读取目录条目，报告 FileId/名称、请求数、吞吐和跨目录可用于核对的 FRN 候选；拒绝重解析目录，不递归跟随，最多四个显式目录、合计 100,000 项，每个同步调用 5 秒取消上限、每个目录 30 秒上限。探针加在完整 harness 的四个 fixture 目录阶段，生产 provider 尚未切换。此段记录首次实现时的状态：当时只有 Rust/PowerShell 语法解析，尚未编译或运行；后续首次提权验证与取消原因见下节。

批量目录探针的 harness 断言进一步加强：除了两个硬链接路径 FileId 相同，还要求目录句柄 FileId 与父目录批次中的目录项 FileId 相同，并与同页 `FSCTL_ENUM_USN_DATA` 的 FRN 匹配；原文件的 FileId 也与 USN FRN/parent FRN 对照。此交叉核对代码已加入，但仍未编译或在 VHD 上执行，只有源码解析检查通过。

VHD harness 已将批量目录 ID 交叉验证拆成独立探针阶段，先于可能失败的可选 `$MFT` file-ID 通道运行；硬链接路径探针和 `$MFT` 探针分开记录，`OpenFileById` 诊断失败不再阻断批量目录和后续初建阶段。目录候选阶段最多读取四个 USN 页，以便匹配低 FRN 的 fixture 项；批量目录名/FileId 的跨目录硬链接及 USN FRN 一致性断言只会在全部证据出现时通过。当前只通过 Rust/PowerShell 语法解析，因最近只读压力 90.05% 仍超线，没有启动 Cargo 编译或 UAC。

### 单次提权 harness 实跑结果与取消（2026-11-04 21:20）

批量目录候选代码首次进入单次 UAC harness。提权前 5 秒预检通过：提交率峰值 70.14%、压力 87.69%、可用内存最低 3.89 GiB；页输入/CPU/磁盘/句柄/池计数器完整。管理员令牌已确认，UAC 仅一次。新 128 MiB VHD 创建、分区、格式化及 fixture 写入后，`probe-volume` 的整机守卫观测到压力峰值 93.37%、可用内存最低 2.10 GiB、页输入峰值 79,156/秒、CPU 均值 20.76%、磁盘 busy 14.98%；System 句柄峰值 2,523,572，分页/非分页池峰值 11.80/5.70 GiB，提交率峰值 71.56%。守卫取消发生在卷打开之前：`native_volume_open_attempts=0`，`directory_batch_requests` 无值，批量目录 API 未执行。VHD 已由 finally 卸载，确认 `Attached=false`，没有留下提升子进程。运行结果记录在 `logs/indexer/host-validation-batched-directory-20261104-01-result.json`。

此次还暴露启动器等待缺陷：管理员子进程已结束且 harness 已写入 failed 状态，`Start-Process -Wait` 父脚本仍存活等待；确认无子进程、无挂载后才中断该等待脚本，并单独恢复记录结果。启动器已改为只轮询返回的管理员子进程 PID，避免 `-Wait` 按后代进程树无限等待；该等待修复已通过 PowerShell 解析，尚无下一次真实 UAC 验证。此轮没有拿到目录批量 API 的 NTFS 结果，预检与 VHD 文件准备开销不能算作索引器扫描成本。

宿主 VHD harness 现增加了“fixture 准备后的六指标复检”：在格式化和写入测试文件之后、第一次 indexer 卷探针之前，再采 5 秒提交率、内存压力、可用内存、页输入、CPU、磁盘活动、System 句柄和两类池计数器；越线时只卸载 VHD 并拒绝卷打开。它补上了首轮实跑暴露的“UAC 前预检通过，但 fixture 准备后资源状态已变化”的窗口。该门控在 21:44 的单次提升运行中通过，运行证据见下一节。

### 单次 UAC：批量目录/FileId 探针实跑（2026-11-04 21:44）

`logs/indexer/host-validation-single-uac-resume-20261104-2145/` 通过 `tools/indexer_host_validation_once.ps1` 执行完整单次提升流程。普通权限六指标预检通过后只触发一次 UAC；管理员子进程日志确认 `is_administrator=true`。同一个提升子进程完成 release 构建、新建 128 MiB VHD、fixture 写入、fixture 后复检及批量目录探针，没有在阶段间再次请求提权。

批量目录 API 已在 NTFS VHD 上实测：4 个显式目录共返回 134 个条目、8 次 API 调用；累计约 1,022 微秒，其中 128 个 `.exe` 所在目录约 293 微秒。`workshop/alias-flowtary.exe` 与 `workshop/Flowtary-1.1.2/flowtary.exe` 返回同一 FileId `281474976710956`；该值也与 USN 中 alias 记录 FRN 相同。三个目录 FileId 同样与 USN FRN 一致。阶段 System 句柄峰值没有增长，分页池约增 64 KiB、非分页池约增 52 KiB；最低可用内存约 5.76 GiB，压力峰值约 81.8%，提交峰值比例约 63.6%。10 秒冷却中 System 句柄净减 12，非分页池净减约 3.68 MiB。窗口较短，不能代替长时资源趋势验收。

本次 harness 在下一条断言停止：旧断言要求 USN 样本中同一个硬链接 FRN 必须带有 `flowtary.exe` 名称。实际 USN 样本只含该 FRN 的 `alias-flowtary.exe`，目录批量 API 则列出了两个路径。这说明 USN 名称样本不能单独证明硬链接名称完整。断言已调整为用 USN 实际返回的 alias 名验证 FRN/FileId 映射，再由跨目录批次证明两个路径的 FileId 相同。修正尚未重跑；本次没有进入初建/恢复阶段。VHD 确认已卸载（`Attached=false`），没有访问物理卷或 C:。

本次证明单次 UAC 方案可以完成同一管理员会话内多个阶段。Windows 只把管理员令牌授予该次启动的提升进程；重新启动另一个管理员测试进程时，仍会有一次 UAC。完整流程内部不会重复弹窗。

结束后的 5 秒 metrics-only 检查保存在 `logs/indexer/post-single-uac-metrics-20261104.jsonl`：压力峰值 78.89%、最低可用内存约 6.67 GiB、提交率峰值 61.49%；System 句柄净变化 0，分页池净减约 0.27 MiB，非分页池净减约 1.10 MiB，计数器错误 0。整机磁盘 busy 平均 22.0%，属于全机背景采样，不能归因于这次已经结束的探针。当前仍有异常高的 System 句柄/内核池绝对基线，因此没有进行物理卷索引。

### 修正断言后的单次 UAC 初建运行（2026-11-04 21:53）

运行目录：`logs/indexer/host-validation-single-uac-resume-20261104-2205/`。提权前预检通过，管理员子进程身份确认成功；整个运行只出现一次 UAC。夹具后六指标预检也通过。批量目录探针列出 134 项，8 次 `GetFileInformationByHandleEx`，每目录总耗时合计约 1.25 ms，API 调用自身累计约 123 μs。129 个 `FindFirstFileNameW/FindNextFileNameW` 路径调用全部成功，用时 54.18 ms；双硬链接文件准确返回两个路径。之前错误断言已通过真实运行。

该轮同样收集了六项整机指标。几个短阶段的提交比例约 59.4%–60.0%，内存压力约 76.1%–78.3%，最低可用内存约 7.1 GiB；System 句柄峰值约 2,523,198，目录及硬链接 API 阶段无正向净增；内核池阶段变化小于约 1 MiB（阶段边界和冷却变化包含全机背景）；探针阶段 CPU 均值约 4%–5%，磁盘 busy 约 0%–3%。初建阶段指标同样纳入报告，但真正采集动作只覆盖数十毫秒并在一项元数据处失败，不能作为全卷扫描成本或稳定性结论。所有计数器均有效，资源守卫未触发。

初建流程成功写出 durable partial checkpoint，重启采集器恢复后继续到硬链接对账。`$Tops` 的不可达 NTFS 元数据记录已按现有规则跳过；接着 `$TxfLog.blf` 和两个 `$TxfLogContainer...` 记录暴露了固定文件名白名单会不断漏掉系统内部记录。实现已改为沿 FRN 父链识别仅限记录号 30、名称 `$TxfLog` 且仍在 `InLostFound` 的隐藏子树。该规则通过下一次真实 VHD 初建运行；随后在受保护的 Windows 文件处遇到新的访问限制，详见下一节。

### `$TxfLog` 子树通过后的受保护文件边界（2026-11-04 22:20）

运行目录：`logs/indexer/host-validation-single-uac-resume-20261104-2230/`。一次 UAC 提升内完成夹具准备、目录/硬链接探针、初建 checkpoint 和恢复尝试。恢复时识别并跳过 4 条确属记录号 30 `$TxfLog` 子树、路径仍在 `InLostFound` 的内部记录；随后首次 `FindFirstFileNameW` 请求在 `System Volume Information/WPSettings.dat` 返回 Win32 error 5。通过 partial checkpoint 的只读解析确认失败 FRN 为 `0x0001000000000025`，父目录 FRN `0x0001000000000024`；它不是 `$TxfLog` 分类错误。没有发布完整快照，VHD 已卸载（`Attached=false`）。

这轮六指标采样在每个阶段均有效。提权前预检提交比例峰值 64.94%、压力 87.19%、可用内存最低 4.05 GiB；夹具后压力峰值 86.62%。目录/硬链接 API 和恢复相关短阶段合计提交比例约 63.2%–63.9%、压力约 83.6%–85.2%、可用内存最低约 4.68 GiB；System 句柄峰值约 2,523,146；阶段内分页池峰值增加约 0.19 MiB、非分页池约 0.43 MiB；CPU 阶段均值最高约 22%，磁盘 busy 阶段均值最高约 16%。探针样本私有内存峰值约 11.6 MiB。System 句柄和池的整机样本含宿主背景，短探针不代表全卷吞吐或稳态成本；没有触发资源保护取消。

同轮目录批量 API 返回 134 项，8 次 API 调用，按目录总耗时约 1.65 ms、API 调用时间 382 μs；129 次硬链接路径 API 成功，用时约 36.8 ms。修改后的 `$TxfLog` 规则现有 273 项工作区测试及 release 构建均通过，真实 VHD 证据确认它能越过此前的事务日志失败点。针对 `System Volume Information` 下受保护文件，现已实现明确的非搜索范围：其目录树不进入查询 posting，目录外硬链接仍保留；完整候选 FRN 集一次预计算，避免为全卷每个文件重复遍历祖先链。该策略有单元回归但还没有新的 VHD 运行验证。完整初建、journal 增量、查询和关闭资源闭环仍未通过。

### 受保护系统目录策略与结束后整机采样（2026-11-04）

`Catalog` 现通过单次子树遍历生成 `System Volume Information` 的紧凑 entry mask，并只把所有链接都在该受保护目录内的 FRN 标记为系统元数据；如果文件有目录外硬链接，则继续索引目录外名称。初建跳过的数量会进入 `initial_protected_system_records_skipped` telemetry 字段。新增回归验证受保护目录项不参与搜索、目录外硬链接仍能返回。`cargo test --locked --manifest-path indexer/Cargo.toml --workspace` 共 274 项通过，`cargo build --release --locked --manifest-path indexer/Cargo.toml` 成功且无编译警告。该逻辑尚未重新通过 VHD 闭环验证。

无卷结束状态样本 `logs/indexer/post-vhd-boundary-metrics-20261104.jsonl` 有 6 个机器样本、计数器错误 0：提交比例峰值 64.61%，压力峰值 85.87%，可用内存最低 4.47 GiB；System 句柄最高 2,523,427、窗口净减 3；分页池约 9.72 GiB、非分页池约 5.58 GiB，窗口峰值分别低于起点约 0.86 MiB、2.50 MiB；CPU 均值 13.27%；整机磁盘 busy 均值 99.71%，吞吐约 20.72 MiB/s，页输入峰值约 5,851/秒。该窗口没有索引进程或卷访问，说明宿主磁盘当时正被其他工作占满，不能归因于 Flowtary。VHD harness 的 90% 磁盘门槛会拒绝这种状态；等整机磁盘活动回落后，再用一个 UAC 跑完整闭环。

### 单次 UAC 的 NTFS 初建、恢复和增量闭环（2026-11-04）

运行目录 `logs/indexer/host-validation-system-volume-policy-20261105-01/` 的状态为 `completed`，退出码 0。`tools/indexer_host_validation_once.ps1` 在普通权限阶段先运行整机预检，然后只启动一个 `RunAs` PowerShell 子进程；建盘、格式化、夹具写入、资源复检、卷探针、初建 checkpoint、恢复、增量、查询、冷却采样及 VHD 卸载都在该提升进程中完成。本次只出现一次 UAC。VHD 已卸载，未枚举或打开任何物理卷。

128 MiB NTFS VHD 的目录批量探针读取 4 个目录、134 项，调用 8 次目录 API；目录累计耗时约 1.233 ms。129 个硬链接路径探针全部成功。初建从 durable checkpoint 恢复并发布 277 项，跳过 4 个符合严格 FRN/名称/父链条件的 `$TxfLog` 内部元数据项，以及 1 个受保护的 `System Volume Information` 项。USN 增量阶段观察到一次变更并发布 278 项；改名、新建、删除及新增硬链接查询断言全部通过。初建硬链接路径 API 为 259 次、约 89.8 ms，单次新增硬链接对账约 871 μs。最终查询返回 4 个硬链接名称；查询耗时分别约 20 μs 和 26 μs。

六项整机观测覆盖每个阶段，但大多数索引阶段只有 2 个、约 20–130 ms 的样本，不能据此估算持续扫描开销。初建/恢复周围的提交比例约 62.5%–62.6%，压力约 82.3%–82.5%，可用内存最低约 5.60 GiB；System 进程句柄峰值 2,523,582，几个采样阶段没有持续正增长；分页池和非分页池阶段变化均在约 0.5 MiB 内。恢复发布阶段整机 CPU 均值约 5.4%、磁盘忙碌约 5.5%，同时约 4.15 MiB/s 聚合 I/O；这些是宿主机指标，不可归因于索引器。进程私有字节峰值约 11.52 MiB。整个过程中资源守卫未触发，所有计数器有效。

结束后的 5 秒 metrics-only 样本有 6 个点、无计数器错误：提交比例峰值 63.4%，压力峰值 83.9%，可用内存最低 5.10 GiB；System 句柄从峰值回落 57，分页池约下降 0.48 MiB，非分页池约下降 1.73 MiB。该结果支持本轮受控 NTFS 功能闭环和“无可见持续句柄增量”，但 128 MiB/278 项规模、短采样及异常的宿主内核资源基线，均不足以证明大规模或长时间资源安全；物理全盘索引仍未验收。

### 单次 UAC 的 30 秒 USN watch 资源观察（2026-11-04）

为把持续运行采样并入同一提权会话，`indexer_host_validation_once.ps1` 和 VHD harness 增加 `-WatchSeconds`，允许 8–30 秒并传给同一提升子进程。索引器 CLI 对单次本地 watch 有 30 秒硬上限；一次误配置为 300 秒的尝试被索引器拒绝，harness 的 `finally` 正常卸载了 VHD。后续将 harness 上限收紧到 30 秒并重跑；第二轮仅弹一次 UAC。

成功运行目录为 `logs/indexer/host-validation-watch-30s-20261104-01/`，状态 `completed`，VHD 确认卸载。28.96 秒 watch 收集 56 个整机样本，完成 28 次 USN 轮询并观察到一次变更；初建恢复为 277 项，增量后为 278 项，查询断言通过。窗口内提交比例 61.64%–62.27%，压力 80.29%–81.44%，最低可用内存约 5.87 GiB；System 句柄 2,523,220 起、峰值 2,523,234、结束 2,523,199；分页池边界净增约 1.78 MiB、非分页池净减约 0.21 MiB；整机 CPU 均值约 9.2%，磁盘 busy 均值约 3.38%，页输入峰值约 553/秒。采样行的必需计数器有效，运行守卫没有触发。以上是整机观测，仍受宿主背景活动影响。

索引器退出后的独立 5 秒冷却样本出现 65,097 页/秒页输入峰值及明显提交量变化，因此这段不纳入索引器 watch 的资源归因。该 30 秒观察只覆盖 128 MiB VHD 和 278 项索引，证明单次提权 watch 闭环及短窗口内没有句柄持续攀升，不是长时间或大规模验收。完整日志在上述目录；未扫描物理卷。

### 10,000 文件目录树探针：资源守卫中止（2026-11-04）

尝试目录树扩展后的运行目录为 `logs/indexer/host-validation-tree-10k-20261104-02/`。本轮只弹一次 UAC，在新 128 MiB NTFS VHD 创建 10,000 个额外小文件并分布到 100 个子目录；VHD 最大扩展到约 26 MiB。资源守卫在 probe 冷却阶段发现整机提交量相对本次 sampler 起点增长超过 1 GiB，于约 16 秒处取消整轮；当时压力约 81%、可用内存约 5.9 GiB，System 句柄峰值较采样基线 +48，分页/非分页池峰值约 +1.6/+5.1 MiB，整机 CPU 均值约 16%、磁盘 busy 约 3%。此提交增长包含宿主背景，不能归因于探针。`finally` 已卸载 VHD，确认 `Attached=false`。

整轮探针累计记录 222 次批量目录 API 请求，包含四个目录交叉验证和整树阶段；旧报告没有分阶段调用数。CLI 旧逻辑在冷却期触发取消时把已完成的探针 JSON 结果覆盖为错误，因此没有保留文件/目录总数，不能据此宣称 10,000 项树遍历完整通过。已修复 CLI：资源守卫取消现在单独报告，保留工作负载阶段的原始结果，同时仍以非零退出码表示整轮被取消。修复后的 Rust workspace 274 项测试通过，Release 构建成功。下一次运行需等宿主提交量增长稳定并通过普通权限六指标预检后再触发一次 UAC。该失败样本作为守卫行为和报告缺陷的证据保存，不用于证明扫描安全或完整性。

卸载后的 10 秒只读恢复采样在 `logs/indexer/post-tree-10k-recovery-report.json`：11 个样本、计数器错误 0；提交比例峰值 67.83%，内存压力峰值 85.59%，可用内存最低 4.56 GiB；System 句柄净增 13；分页池净增 20.36 MiB，非分页池净减 0.33 MiB；CPU 加权均值 22.55%，磁盘 busy 均值 11.73%，聚合读写约 4.99 MiB/s，页输入峰值约 2,105/秒。此恢复窗口没有卷访问，指标是整机背景状态，不能归因给刚结束的 VHD 操作。

### 本轮只读整机基线（2026-11-04 23:58）

`logs/indexer/goal-host-metrics-20261104-235802/report.json` 使用 release 索引器仅采集整机计数器 20 秒，没有提权、卷打开、目录枚举或索引工作；21 个样本的必需计数器错误为 0。整机指标：提交量峰值 52.73 GB（提交上限比例峰值 67.51%）；内存压力峰值 86.13%，可用物理内存最低 4.39 GiB，页输入峰值 31.1 页/秒；System 句柄峰值 2,523,098、窗口净变化 -15；磁盘 busy 平均 1.02%、估算读写合计约 0.14 MB/s；分页池峰值 10.56 GB、非分页池峰值 6.06 GB（窗口分别净减约 0.25 MiB / 2.69 MiB）；CPU 平均 3.83%。索引器进程私有字节峰值约 9.54 MiB、CPU 平均约 0.06%。

此报告是无索引负载的整机基线；它确认当前宿主仍有异常高的 System 句柄和内核池绝对值，不能把这些占用归因给 Flowtary。资源指标没有显示本轮采样自身造成明显净增长，但 20 秒空闲采样不能证明长时间运行或真实卷扫描安全。由于宿主压力为 86.13%，本轮没有申请 UAC 或启动 VHD/物理卷探针。

### 单次 UAC 启动被取消（2026-11-05 00:13）

`tools/indexer_host_validation_once.ps1` 针对新目录 `host-validation-tree-10k-20261105-01` 完成 5 秒普通权限六指标预检，结果 `accepted-before-uac`、无拒绝原因；随后只调用一次 `RunAs`。Windows 返回“操作已被用户取消”，记录在 `host-validation-tree-10k-20261105-01-result.json`。没有管理员 token/child 记录，没有 harness 目录、VHD、`flowtary-indexer` 或 `diskpart` 子进程。该次不构成 VHD 验证；本轮未重试 UAC。

### 目录批量硬链接补全 provider（2026-11-05，待 VHD 实测）

新增 opt-in `directory_link_walk` 初建路径：当授权 VHD manifest 同时启用 `force_enum_usn` 和 `directory_link_walk` 时，枚举 catalog 中唯一目录 FRN，使用 `GetFileInformationByHandleEx(FileIdBothDirectoryInfo)` 读取批量名称/FileId，并以 parent FRN 直接补齐硬链接链。它避免在初建阶段对每个普通文件调用一次 `FindFirstFileNameW`。每次只打开一个目录句柄；限制为 120 秒和 1,000,000 个目录批次；每批回调检查点，按配置间隔落盘私有检查点，恢复时从检查点中的当前目录开头重放，依赖 catalog 的 add-only 幂等链接合并；跳过 reparse 目录与 System Volume Information，普通目录无法解析或打开时失败关闭。VHD harness 在该模式下把故障注入停点移至首个持久目录批次检查点，并要求报告证明至少打开过目录、读取过目录项、无逐文件硬链接 API 调用、目录句柄活动数回到 0 且峰值活动数为 1。新计数器记录目录句柄打开/活动/峰值数、目录项数和跳过的 reparse 点。

这条路径仍只限经身份核验的新建测试 VHD，尚未启用为生产初建 provider。本机本轮唯一 UAC 被取消，故未运行该 VHD provider；真实 FileId/FRN 完整性、目录移动期间的竞态、检查点恢复、USN replay、minifilter 资源曲线和全盘规模仍待验证。离线 Rust workspace 34 项测试通过，release 构建成功，PowerShell harness 解析通过；新增 RAII 句柄追踪器可报告打开数、活动数和峰值活动数，离线测试验证了 Drop 后活动数归零，但这些验证不能替代 NTFS VHD 运行证据。

### 目录 provider 构建后只读整机观察（2026-11-05）

`logs/indexer/post-directory-provider-metrics-20261105-01/report.json` 为 15 秒 metrics-only 采样，目录 provider 未运行、卷打开/目录枚举均为 0，16 个观测样本的计数器错误为 0。提交量相对首样本峰值增长 74.61 MiB，峰值提交比例 62.39%；内存压力峰值 75.93%，可用内存最低 7.61 GiB；System 句柄从 2,523,025 到窗口峰值 2,523,081（+56）；分页池峰值 9.89 GiB、相对首样本 +1.08 MiB，非分页池峰值 5.66 GiB、+5.03 MiB；CPU 加权均值 7.19%；磁盘 busy 加权均值 4.22%、吞吐约 0.92 MiB/s；页输入峰值约 151/秒。indexer 私有内存约 9.23 MiB、CPU 均值约 0.07%。句柄及机器级资源变化包含后台活动，不能归因于构建或 Flowtary；此采样不构成 NTFS 扫描负载结果。

## 2026-11-05：单次提升前预检与 Rust 验证

运行目录前缀：`logs/indexer/host-validation-tree-10k-20261105-010139`。一次性入口先完成 5 秒 metrics-only 预检，`accepted-before-uac`；这段没有卷访问。提交量从 45.45 GiB 起始，峰值增加 21.79 MiB（提交率峰值 62.51%）；物理内存压力 75.62%–75.68%，最低可用约 7.69 GiB；System 句柄 2,523,018 起始、峰值 2,523,027（+9）；分页池约 9.75 GiB、窗口峰值增加约 0.01 MiB，非分页池约 5.64 GiB、峰值增加约 0.73 MiB；整机 CPU 加权均值 4.30%，磁盘 busy 1.08%、平均读写约 0.22 MiB/s，页输入峰值约 17/秒，计数器错误 0。metrics 采样器私有内存峰值约 9.32 MiB、236 个进程句柄。System 句柄和两类池的绝对基线仍异常，因此该预检只证明相对资源门槛通过，不证明宿主基线健康。

随后仅请求了一次 UAC。Windows 返回“操作已被用户取消”，没有启动提升子进程；没有创建/挂载 VHD、访问卷或启动索引负载。没有再次请求提权。随后无需管理员权限的 Rust workspace 验证通过：279 项测试（35 个 indexer、230 个 goz-core、2 个集成测试、11 个 goz-winfs、1 个 doctest），Release 构建成功。源码验证通过不代表真实 NTFS 初建或过滤驱动栈开销已验收。

### 目录 provider 10,000 项单次 UAC 尝试（2026-11-05）

运行目录 `logs/indexer/host-validation-dirwalk-10k-single-uac-20261105-020000/`。将目录批量链接遍历、10,000 个夹具文件、checkpoint/resume、30 秒 USN watch 和整机资源采样合并到同一个 `indexer_host_validation_once.ps1` 调用；普通权限预检覆盖 5.02 秒、6 个 machine-observation 样本，计数器错误为 0。提交量峰值约 45.42 GiB（相对预检首样本 +9.45 MiB），内存压力最高 75.87%，最低可用物理内存约 7.63 GiB；System 句柄从 2,522,966 增至峰值 2,522,972（+6）；分页池约 9.75 GiB、窗口峰值相对首样本无增量，非分页池约 5.64 GiB、峰值减少约 0.10 MiB；CPU 均值约 3.56%，磁盘 busy 均值约 0.96%，页输入峰值约 5.84 页/秒。该预检通过相对门槛，但报告仍标记 System 句柄、分页池和非分页池的绝对基线异常，因此不表示主机内核状态健康。

预检后只发起了一次 RunAs。Windows 返回“操作已被用户取消”，记录在同目录旁的 `host-validation-dirwalk-10k-single-uac-20261105-020000-result.json`；没有提升子进程 token 记录、harness state、VHD、DiskPart/索引器进程或卷访问。没有重试 UAC。故本次没有验证真实 NTFS provider、checkpoint/resume、USN watch 或过滤驱动栈资源曲线；单次提升入口只需一次授权即可串行执行上述完整套件，后续独立调用仍会重新要求授权。

### 单次 UAC 验证批次尝试（2026-11-05 03:28）

运行目录：`logs/indexer/host-validation-20261105-032853/`。单次入口先完成 5.02 秒、6 个样本的 metrics-only 预检，计数器错误 0，达到启动门槛。整机提交量基线约 46.15 GiB、窗口峰值增加 0.97 MiB；内存压力 78.88%–78.89%，最低可用内存 6.67 GiB；System 句柄基线 2,523,255、峰值 +3、结束 -2；分页池约 9.80 GiB、非分页池约 5.66 GiB，窗口内采样峰值未高于基线；CPU 均值 3.65%，磁盘 busy 均值 0.70%、读写均值约 0.16 MiB/s，页输入峰值约 7.84/秒。

预检通过后只请求一次 UAC；Windows 返回“操作已被用户取消”。没有提升子进程、VHD、DiskPart、索引器卷访问或新的测试结果；没有再次请求提权。该批次不是 provider 测量。若后续执行，三个场景仍可在同一个管理员会话中串行完成，批次内部不需要重复 UAC。

### 单 UAC 多 provider 批次入口（2026-11-05）

为避免对 File Layout、USN fallback、目录批量 provider 分别启动提升进程，`indexer_host_validation_once.ps1` 增加串行场景矩阵：默认依次运行 `file-layout`、`usn-fallback-resume` 和 `directory-link-walk`，每个场景在独立的新 VHD 运行目录内执行，并沿用 VHD harness 自身的资源预检、工作负载采样及卸载逻辑。任一场景失败后停止余下场景。10,000 文件规模选项只传给目录遍历场景。普通权限启动时脚本只有一个 `-Verb RunAs` 调用点；若由已提升的 PowerShell 启动，则用 `UseShellExecute=false` 创建继承当前管理员令牌的子进程，不再触发 RunAs/UAC。`-PlanOnly` 可在不采样、不访问卷、不请求 UAC 的情况下预览默认批次或逗号分隔的场景子集。

当前验证仅覆盖 PowerShell AST 解析、默认/子集 `-PlanOnly` 输出、10,000 文件与 30 秒参数，以及脚本中唯一 RunAs 调用点检查；没有重新启动 UAC。此前 UAC 取消的结果仍是本机最新授权运行结果，因此批次串行逻辑尚无管理员运行时证据，不能宣称 provider 验收已完成。

### 单 UAC 批次复核（2026-11-05 04:04）

再次启动 `host-validation-one-uac-20261105-050000`，包含三个 provider 场景、10,000 项目录夹具和 30 秒 watch。唯一一次普通权限预检通过，随后只调用一次 `Start-Process -Verb RunAs`。预检的六项机器指标为：整机提交量峰值约 46.18 GiB（约 63.50% 提交上限，窗口变化 −15.65 MiB）；内存压力峰值 78.60%，最低可用内存约 6.78 GiB；System 句柄 2,523,202 至窗口峰值 2,523,215（+13）；分页池峰值约 9.81 GiB、相对首个观测 +0.29 MiB，非分页池约 5.66 GiB、+3.87 MiB；CPU 均值 3.37%；磁盘 busy 均值 1.29%、吞吐均值约 0.17 MiB/s，页输入峰值约 15.59 页/秒。计数器错误为 0。该预检本身没有卷访问。

Windows 随后再次将这一条 UAC 请求返回为“操作已被用户取消”。没有管理员子进程 token、VHD、DiskPart、indexer 卷操作或 harness state，因此以上仅是空闲预检，不能归因于索引器，也没有任何 provider 负载数据。系统基线仍标记异常：System 句柄约 252 万、分页池约 9.81 GiB、非分页池约 5.66 GiB。批次启动器只有一个 RunAs 调用点；一旦这次提示被确认，三个场景会共用同一个提升会话，不会在场景之间重复 UAC。Windows 不提供把此同意永久授予未来独立进程的安全方式；未授权前不能代替用户操作该安全提示。

### 默认三 provider 批次：单次提升请求被取消（2026-11-05）

`logs/indexer/host-validation-batch-3providers-10k-20261105-021029-launch.json` 记录了默认三场景批次和 10,000 项目录夹具；`-PlanOnly` 已确认规划为 `file-layout`、`usn-fallback-resume`、`directory-link-walk`，watch 30 秒。普通权限预检有 6 个观测样本、计数器错误为 0，故只发起了一次 RunAs。提交量约 45.51 GiB、相对首样本峰值变化 −0.32 MiB；内存压力 76.48%–76.49%，最低可用约 7.44 GiB；System 句柄基线/峰值 2,523,017，窗口末端减少 29；分页池峰值约 +0.02 MiB、窗口末端约 −5.39 MiB；非分页池峰值约 +0.004 MiB、末端约 −2.60 MiB；整机 CPU 均值 6.12%，磁盘 busy 均值 4.32%、读写合计均值约 3.12 MiB/s，页输入峰值约 535/秒。该预检门槛通过，但仍报告宿主 System 句柄和池绝对基线异常。

随后 Windows 再次返回“操作已被用户取消”，见 `host-validation-batch-3providers-10k-20261105-021029-result.json`。没有管理员 token、批次 state、场景 harness、VHD、DiskPart 或 indexer 子进程；没有卷访问或索引工作。本次没有重试。新批次的脚本解析、默认矩阵/逗号分隔子集 `-PlanOnly`、资源参数和唯一 RunAs 调用点已验证；三 provider 管理员运行时结果仍待一次成功授权。

### 600 万离线容量后重新请求一次 VHD 批次（2026-11-05）

`host-validation-batch-20261105-030100` 的 5 秒整机预检通过：压力峰值低于 80%、可用内存高于 6 GiB、System 句柄/内核池增量无越限，计数器完整。预览仍是三场景单 UAC 矩阵；之后唯一 `RunAs` 再次以“操作已被用户取消”失败。目录仅有 preflight 和 launch/result 记录，没有 elevated token、harness state、VHD、DiskPart、卷访问或 provider 工作。不要把这个批次报告为 NTFS 验收。当前权限请求需要桌面用户在 UAC 安全提示中确认；脚本不会尝试模拟确认或降低系统安全设置。

### 当前工作树的离线验证（2026-11-05）

本轮 `cargo test --workspace --locked` 通过：41 个 indexer 测试、230 个 goz-core 测试、2 个集成测试、13 个 goz-winfs 测试和 1 个 doctest，共 287 项。新增 named-pipe 测试验证 SID-scoped DACL、注入拒绝、真实客户端 SID 匹配/不匹配以及恢复服务线程身份。随后 `cargo build --release --locked` 通过且无警告。该验证没有打开卷；不能证明上面的 directory-link provider 已在 NTFS 上运行。

### 受限 VHD provider 批次再次取消（2026-11-05）

运行目录：`logs/indexer/service-development-vhd-20261105-045809*`。一次性批次脚本的 5 秒预检通过，指标计数完整，然后唯一一次 RunAs 返回“操作已被用户取消”。没有管理员子进程、批次 state、VHD、DiskPart 或卷访问。此处记录为授权未发生，绝不把它当作 provider 结果；本轮不再请求第二次 UAC。批次参数应以单个逗号分隔字符串传入 `-Scenarios`。

### directory-link-walk 10,000 项批次：唯一 UAC 请求取消（2026-11-05 05:54 UTC）

计划运行目录 `logs/indexer/goal-directory-link-walk-10k-20261105-060000/`。`-PlanOnly` 确认只选择 `directory-link-walk`、10,000 项夹具和 30 秒 USN watch，计划 UAC 请求数为 0、卷访问为 false。正式入口完成 5 秒六指标预检，计数器错误为 0：提交率峰值 63.76%；内存压力峰值 79.34%、可用内存最低约 6.53 GiB；System 句柄 2,523,206 起、峰值 2,523,207；分页池窗口峰值约 9.83 GiB、非分页池约 5.66 GiB；整机 CPU 均值 4.12%，磁盘 busy 均值 0.76%、吞吐约 0.14 MiB/s，页输入峰值约 14.8 页/秒。绝对 System 句柄/内核池基线仍异常，报告仅将其作为相对预检记录。

预检后只发起一次 `RunAs`。Windows 返回“操作已被用户取消”，见同目录 `*-result.json`；没有管理员子进程 token、批次 state、VHD、DiskPart、卷打开或索引工作。此次不产生 provider 验收数据；该 UAC 请求没有重试。

### 普通目录上的 FileId 批量 API 六指标实测（2026-11-05）

为不再重复请求 UAC，新增独立 `directory-api-probe.exe` 和 `directory_api_bench.ps1`。脚本在本仓库日志目录创建带标记的 10,000 个零字节 `.exe` 和 100 个子目录夹具，探针只接受该标记根；执行限额为 30 秒、100,000 项、10,000 目录、深度 32。探针只打开目录句柄，使用 `GetFileInformationByHandleEx(FileIdBothDirectoryInfo)` 取名称和 FileId，逐目录用 FileIdInfo 再核对身份；不打开普通文件、不读文件内容、不打开卷设备，跳过 reparse 子目录。整机指标 sampler 先空闲 3 秒，随后覆盖扫描和剩余观察窗口；结束后夹具自动清除，报告/原始样本保留。

成功复测：`logs/indexer/directory-api-10k-repeat-20261105-062000/report.json`，原始 `samples.jsonl`。101 次目录打开，202 个批次，完整返回 10,100 项（100 目录、10,000 文件），FileId mismatch 为 0；扫描墙钟 124.5 ms，其中 API 累计 97.2 ms。目录句柄峰值活动数为 1、结束活动数为 0；探针进程句柄 124→124，私有字节约 1.36→1.43 MiB，整段观察的进程 CPU 均值约 0.031%。

同一 18 秒整机窗口计数器错误为 0：提交量相对首样本峰值约 +101.8 MiB、窗口结束约 +34.7 MiB；压力峰值 79.68%，可用内存最低约 6.42 GiB；System 句柄首样本 2,523,234、峰值 +22；分页池峰值约 +21.8 MiB、非分页池约 +1.24 MiB；整机 CPU 加权均值 4.61%，磁盘 busy 0.99%、读写合计约 0.25 MiB/s，页输入峰值约 47/秒。首次遍历的原始 1 秒样本也完整保留：两次运行的分页池峰值分别在观测窗口起点后 4.08/4.09 秒，较窗口首样本增加约 21.5/21.8 MiB，并在约 6 秒时回到基线附近；两次整机提交量峰值分别约 +81.1/+101.8 MiB，出现在窗口起点后 15.32/5.12 秒。扫描信号在观测窗口开始约 3 秒后发出，故这些峰值与扫描时间有相邻关系，值得在长批次上继续观察；但采样间隔大于 124 ms 工作负载且整机存在后台活动，不能仅凭相关性归因给探针。System 句柄峰值一次在扫描信号前约 1 秒，另一次在信号后约 12 秒。该测试只逐目录核验目录 FileId，普通文件 FileId 仅由批量 API 返回、未逐个另行打开校验；它不能证明小型目录读取对 wcifs/Filter Manager 的长期或全盘成本为零。

这个结果证明当前 Windows 批量目录 API 探针在普通 NTFS 文件夹上能列全名称并保持单目录句柄生命周期；它不是授权 VHD 上的卷级 USN/FRN 闭环，也不能替代 10,000 项目录 provider 的 VHD 恢复测试。`directory-api-probe` Release target 构建成功，PowerShell AST 解析通过。首次运行仅因脚本将空 JSON 对象的 PowerShell `.Count` 误读为 1 而未生成汇总报告；原始探针数据完整且夹具被清理，修正后第二次成功报告如上。

### System 句柄快速增长熔断（2026-11-05）

`Recorder::start_guarded` 仍保留单轮累计 System 句柄增长 +512 的原上限；新增一秒级速率守卫：连续两个常规采样间隔都超过 +120 个 System 句柄/秒时，立即取消当前工作负载。此阈值低于用户观测的扫描增长（两轮约 +150/秒、+135/秒），目标是在累计阈值触发前中止持续增长；暂停/边界的短间隔采样不参与速率判断。资源取消仍通过共享 cancellation token 传给 provider 的同步 I/O 取消守护。

阈值回看了本机已有 86 份 JSONL 机器采样、2,785 组相邻 0.75–1.5 秒样本：出现过一次约 +128/秒的孤立背景尖峰，没有连续两个间隔超过 +120/秒。因此使用“两次连续”而非单次尖峰取消。这个样本集有限，不能保证其他工作负载不会连续越线；触发后会保守停止索引工作并保留 guard 违规记录。新增速率函数覆盖了快速增长、较低增长、句柄下降和过短采样间隔；状态机测试还验证单次尖峰不会触发、连续两次会触发、恢复正常后计数清零。当前 workspace 测试已通过 38 个 indexer、230 个 goz-core、2 个集成、11 个 goz-winfs 和 1 个 doctest，共 282 项，Release 构建成功。此改动尚未重新部署，也没有在真实卷负载下触发验证。

### directory-link-walk 10,000 项单 UAC 批次：预检通过、授权取消（2026-11-05 06:20）

计划目录 `logs/indexer/host-validation-20261105-062022/`，仅运行 `directory-link-walk`、10,000 项 fixture 和 30 秒 USN watch。`-PlanOnly` 确认计划 UAC 请求数为 0、卷访问为 false。普通权限预检覆盖 5.02 秒和 6 个 machine-observation 样本，计数器错误为 0：整机提交量峰值约 46.45 GiB、提交率 63.85%；内存压力峰值 79.36%、可用内存最低约 6.53 GiB；System 句柄基线/峰值为 2,523,193、增量 0；分页池约 9.83 GiB、窗口峰值相对首个样本无增长，结束减少约 1.38 MiB；非分页池约 5.65 GiB、窗口峰值无增长，结束减少约 1.51 MiB；整机 CPU 加权均值约 3.71%，磁盘 busy 均值约 1.35%、读写均值约 0.12 MiB/s，页输入峰值约 1.07 页/秒。Flowtary 预检进程私有字节约 9.44 MiB、句柄 237–238。主机绝对 System 句柄及两类内核池基线仍被标记异常。

预检通过后只调用了一次 `RunAs`；Windows 返回“操作已被用户取消”，见同目录旁的 `host-validation-20261105-062022-result.json`。没有提升子进程 token、VHD、DiskPart 或 indexer 卷操作，故没有 provider/workload 数据，也没有重试 UAC。原始六指标样本和报告保存在同名 `*-preflight/` 目录。workspace `cargo test --workspace --locked` 本轮通过 44 个 indexer、230 个 goz-core、2 个集成、13 个 goz-winfs 测试和 1 个 doctest，共 290 项；C++ Release 构建后 CTest 8/8 通过。为恢复 C++ 构建，UI 测试加入了新预热接口的本地 mock。离线测试不能替代本次尚未发生的 VHD 验收。

### 卷租约扩展到服务共享命名空间（2026-11-05）

实验性 `collect`/`probe-volume` 现在在打开卷前同时取得 LocalAppData 私有租约和 ProgramData 共享租约；服务未来的 writer 有对应的 ProgramData 租约入口。测试证明服务侧已有租约时用户 worker 会在打开卷前被拒绝，用户 worker 持有共享租约时服务侧同样不能取得第二份。另有子进程测试证明独占句柄在进程间阻止并发打开，持有者退出后租约可重新取得。目录在服务安装时由 SYSTEM/Administrators ACL 保护。当前只读 query service 尚无 collection command，故服务侧入口尚未由运行服务调用；真实服务 IPC、采集任务生命周期和 VHD 交叉进程实测仍未完成。`cargo test --workspace --locked` 的 290 项通过及 `cargo build --release --locked` 成功；这些离线证据不表示已经安装服务或打开卷。

### VHD 尝试后的 30 秒空闲六指标参考（2026-11-05 06:30）

`logs/indexer/goal-idle-followup-20261105-063000/` 只运行 metrics sampler，没有卷访问或目录遍历。31 个 machine-observation 样本覆盖 30.02 秒，计数器错误 0。提交量基线约 46.42 GiB，窗口峰值 +11.17 MiB、结束 −27.23 MiB；内存压力峰值 79.35%，可用内存最低 6.53 GiB；System 句柄 2,523,206 起、峰值 +1、结束 −13；分页池峰值 +0.96 MiB，非分页池峰值 +4.72 MiB；整机 CPU 加权均值 3.64%；磁盘 busy 加权均值 0.88%，读写合计约 0.147 MiB/s，页输入峰值 16.73 页/秒。Flowtary sampler 私有字节约 9.78–9.84 MiB、进程句柄 237–238。System 句柄和两类池的绝对基线仍异常；该空闲窗用于后续 workload 的背景参照，不是健康基线或 provider 安全证明。

### 10,000 项 VHD 批次的单次授权等待与快照写入上限修正（2026-11-05）

运行目录计划为 `logs/indexer/host-validation-20261105-065410/`，场景为 `directory-link-walk`、10,000 项夹具、30 秒观察。正式入口完成 5 秒、6 个整机样本的预检，计数器错误为 0：提交率峰值 64.04%、内存压力最高 79.83%、可用内存最低 6.85 GiB、System 句柄最高 2,523,231（窗口增量 0）、分页池峰值约 9.85 GiB、非分页池约 5.66 GiB、CPU 均值 4.92%、磁盘 busy 均值 0.61%、页输入峰值约 0.98 页/秒；提交量窗口峰值约 46.58 GiB。绝对句柄/内核池基线仍异常。Windows 对唯一一次 `RunAs` 返回“操作已被用户取消”，见 `host-validation-20261105-065410-result.json`。没有管理员 token、VHD、DiskPart 或卷打开；此轮不是 provider 验收，也没有再次请求 UAC。

为避免重复弹窗，用户可在一次手动打开的管理员 PowerShell 会话中直接运行：`pwsh -NoProfile -File "C:\Users\ted\Desktop\flowtary\tools\indexer_host_validation_once.ps1" -ElevatedChild -Scenarios directory-link-walk -ScaleFixtureCount 10000 -WatchSeconds 30`。提升后的 harness 会在创建/格式化 VHD 前再做六项整机资源预检；该命令在已有管理员令牌下不调用 `RunAs`。

本轮还将快照文件 512 MiB 上限从“临时文件写完后检查”前移为每个编码块写入前检查，并在预算内预留 CRC 尾部，避免超限时临时快照继续扩张。新回归确认超限数据不会写入；`cargo test --workspace --locked` 通过 291 项，`cargo build --release --locked` 成功。该软件侧改动没有卷访问，不改变尚未通过的 NTFS provider 验收状态。

### 一次性管理员批次尝试及 10,000 项目录 API 观测（2026-11-05）

`logs/indexer/host-validation-20261105-073511*` 记录了三个 VHD provider 场景（其中目录遍历规模为 10,000 项）的单次批次尝试。5 秒普通权限预检通过，6 个整机样本的计数器错误为 0：提交量约 46.64 GiB，窗口峰值没有高于首个样本；内存压力峰值约 80.09%、可用内存最低约 6.30 GiB；System 句柄基线 2,523,204、峰值 +3；分页池约 9.82 GiB、非分页池约 5.66 GiB，池峰值相对首个样本分别仅 +0.004/+0.133 MiB；整机 CPU 加权均值约 3.54%；磁盘 busy 均值约 1.04%、读写约 0.11 MiB/s，页输入峰值约 0.97 页/秒。句柄和池的绝对基线仍标记异常。Windows 对唯一一次 `RunAs` 返回“操作已被用户取消”，见 `host-validation-20261105-073511-result.json`；没有提升子进程、VHD、卷访问或 provider 工作，也没有重试 UAC。因此本批次不是 VHD/NTFS 验收。

同轮执行了不需管理员权限的 `tools/directory_api_bench.ps1 -Entries 10000 -Directories 100`，报告和原始样本保存在 `logs/indexer/directory-api-10k-20261105-074000/`。测试树包含 10,000 个空 `.exe`、100 个子目录，探针完整返回 10,100 项，FileId 不匹配 0；101 次目录打开，最大同时活动目录句柄 1、结束活动数 0；探针进程句柄 124→124，私有字节约 1.36→1.43 MiB。目录遍历墙钟约 114.4 ms，累计文件系统 API 时间约 91.7 ms。它不打开卷设备、普通文件或文件内容，故只证明受控目录 API 的小型用户态夹具完整性/句柄平衡，不代表全盘索引 provider 或 `wcifs.sys` 成本。

六指标采样覆盖完整 18.02 秒窗口（探针在其中约 114 ms，窗口含启动前等待及后续空闲），计数器错误 0：提交量峰值相对首点 +252.0 MiB、内存压力 80.32%–81.08%、可用内存最低约 5.98 GiB；System 句柄峰值 +47；分页/非分页池峰值相对窗口起点约 +2.25/+1.89 MiB；整机 CPU 加权均值 6.61%；磁盘 busy 均值 6.19%、总读写约 6.09 MiB/s、页输入峰值约 5,572 页/秒。该 18 秒窗主要是夹具已生成后的等待/冷却期，且含主机背景活动；这些整机变化不能归因于 114 ms 探针。实际探针执行期间六项指标没有足够时间分辨率可单独量化，这是下一版测量 harness 的限制，不能把全窗数值当成扫描增量。

### 10 秒重复目录 API 扫描及六指标窗口（2026-11-05）

为让受控目录遍历持续足够长以观察资源趋势，`directory_api_probe.cpp` 与 `directory_api_bench.ps1` 新增可选 `-RepeatSeconds`：重复遍历同一带标记的静态树，每轮重新验证目录 FileId 和条目数，累计 API 时间与轮数；每次运行最多 20 秒、累计最多 2,000,000 项、最多 200,000 个目录、同时最多一个活动目录句柄。构建 `cmake --build build --config Release --target directory-api-probe` 成功。运行命令为 `pwsh -NoProfile -File tools/directory_api_bench.ps1 -Entries 10000 -Directories 100 -RepeatSeconds 10 -OutputDirectory logs/indexer/directory-api-10k-repeat10-20261105-080000`。

探针在 10.007 秒内完整通过 89 轮、898,900 项，其中 890,000 个 `.exe`，FileId 不匹配 0；累计 17,978 个 API 批次、目录 API 时间 8.205 秒。8,989 次目录打开的活动句柄峰值为 1，结束活动句柄为 0；进程句柄 124→124，私有字节约 1.35→1.42 MiB。探针不打开原始卷设备、普通文件或文件内容。每轮完整性检查通过，未因条目/时间预算提前停止。

完整 18.016 秒整机采样窗覆盖约 3 秒启动等待、10 秒扫描和约 5 秒尾部观察；计数器错误 0。提交量峰值相对采样首点下降约 585.6 MiB；压力约 82.06%–82.07%，可用内存最低 5.67 GiB；System 句柄窗口起点约 2,523,516、峰值持平、结束下降 176；分页池峰值相对首点约 +2.00 MiB、非分页池无正增长（末端约 −2.77 MiB）；整机 CPU 均值 7.29%，探针进程 CPU 均值约为单逻辑处理器的 2.43%；磁盘 busy 均值 3.49%、读写约 0.90 MiB/s，页输入峰值 166 页/秒。提交量与池/句柄的净下降表明本窗口有明显背景波动；故这些值只能作为整机观测，不能把变化归因给扫描或据此证明过滤驱动资源安全。更关键的是，该试验验证的是固定夹具的目录枚举 API，不是全盘 USN/MFT provider；VHD 授权批次仍未运行。

### 扫描—暂停—恢复对照及秒级资源对齐（2026-11-05）

为排除尾部空闲期对峰值的掩盖，探针开始/结束时记录 Unix 毫秒时间戳，harness 将一秒整机样本对齐到扫描阶段，报告阶段前基线、峰值和结束值；阶段边界容差为一个采样间隔。另增加 `-ActiveSeconds/-PauseSeconds/-ResumeSeconds`，用于同一进程和同一静态夹具的扫描 5 秒、暂停 5 秒、恢复扫描 5 秒对照；暂停期间没有目录 API 请求。构建成功，批次通过两段扫描各 5 个样本、计数器错误为 0 的断言。原始数据与阶段报告位于 `logs/indexer/directory-api-10k-spr-20261105-083000/`。

此轮完成 46 轮初扫和 46 轮恢复扫描，共 929,200 项；活动目录句柄峰值 1、结束 0，进程句柄 124→124，FileId 不匹配 0。以每阶段扫描前最近样本为基线，初扫阶段 System 句柄峰值/结束 +1/+1，分页池峰值 +22.05 MiB、阶段结束 −0.38 MiB；暂停阶段 System 句柄峰值 +7、结束 0，分页池额外峰值仅 +0.01 MiB、结束 −0.34 MiB；恢复阶段 System 句柄峰值 +2、结束 +1，分页池峰值/结束 +0.19/+0.19 MiB。初扫时分页池曾短暂升高，随后在扫描仍运行时回落；暂停和恢复阶段没有复现同量级的池峰值。这不支持“每次遍历都持续增加句柄/池”的说法，但单轮测试也不足以排除缓存效应、宿主干扰或别的目录形状问题。

六项同阶段读数还包括：初扫提交量峰值/结束 +55.4/+21.9 MiB、CPU 均值 8.04%、磁盘 busy 0.76%、I/O 约 0.16 MiB/s；暂停阶段提交量峰值/结束 −6.17/−50.63 MiB、CPU 4.87%、磁盘 busy 0.89%、I/O 约 0.10 MiB/s；恢复阶段提交量峰值/结束 +7.52/+5.36 MiB、CPU 7.04%、磁盘 busy 1.39%、I/O 约 0.17 MiB/s。各阶段压力峰值在约 80.48%–80.68%，可用内存最低约 6.11 GiB；页输入峰值分别约 4.86、0.97、4.93 页/秒。System 句柄峰值和内核池阶段变化包含宿主所有工作；即使在同进程控制实验里，也不能把一条短时曲线等同于 `wcifs.sys` 标签归因。主机 System 句柄及分页/非分页池绝对基线依旧异常。

之后两次独立的 10 秒重复扫描都观察到初始阶段分页池短时峰值约 +22.66/+24.55 MiB，但在扫描结束采样回到基线；System 句柄峰值变化分别为 +12 和 0。结合扫描—暂停—恢复结果，下一步需要在重复的冷/暖缓存轮次中增加足够长的停扫回落观察，并采集 pool-tag/Filter Manager 证据；当前只能确认这一小型目录 API 路径未表现为扫描次数线性累积的句柄增长，不能外推到完整全盘索引。

同一 5–5–5 秒序列再重复一次，报告和样本在 `logs/indexer/directory-api-10k-spr-repeat2-20261105-084000/`。再次完成 46+46 轮、句柄峰值 1 / 结束 0、进程句柄保持 124，计数器错误为 0。初扫分页池峰值 +23.22 MiB、阶段结束 +0.11 MiB；暂停阶段峰值 +0.03 MiB、结束 +0.03 MiB；恢复阶段峰值 +0.41 MiB、结束 −0.20 MiB。System 句柄各阶段峰值/结束分别为 +1/+1、0/−1、+4/+4。初扫的分页池瞬时峰再次出现，暂停和恢复没有等量重复；这进一步支持“存在一次性/短暂峰值”的观察，但还不能区分文件系统缓存、minifilter 或整机后台噪声。六指标各阶段数据已经纳入 `report.json` 的 `phase_aligned_observations`；整段含暂停的 envelope 单列，不再冒充纯扫描窗口。

### Pool tag / allocation stack ETW 采集准备（2026-11-05）

本机安装的 WPT 支持 `xperf -a pool -pooltags`，WPR 内置 `Pool.Verbose` 也包含 PoolAlloc/PoolFree 栈，但默认 verbose 配置列出 1 MiB 缓冲 × 1,618 个，可能显著改变本机的提交量与内核池，所以不使用该默认缓冲配置。VHD harness 改用 WDK `tracelog.exe` 建立随机名称的 `SystemTraceProvider` logger：`PROC_THREAD+LOADER+POOL`、PoolAllocation/PoolFree stackwalk、64 KiB buffer、8–64 buffers、128 MiB circular ETL；停止后运行 xperf pool-tag 摘要。Microsoft 文档支持用 `tracelog -start <name> ... -eflag ...` 建立非 NT Kernel Logger 的 SystemTraceProvider session，并设置 system logger 模式。[SystemTraceProvider session 文档](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-a-systemtraceprovider-session) 此路径用于和本机已存在的 `NT Kernel Logger` 并存；只停止本次创建的唯一 session，不改动 Windows 已有 logger。批次内部不另行请求 UAC。

本轮普通权限启动尝试未产生 ETL：xperf 报 `NT Kernel Logger ... (0xb7)`，之后确认 `xperf -loggers` 为 `No Active Loggers`，输出目录没有 trace 文件，失败路径也清理了 fixture。随后只发起了一次管理员 PowerShell 启动请求；Windows 返回“操作已被用户取消”，没有提升进程、trace logger 或测试目录，未再次请求 UAC。当前不能声称拿到了 pool tag 或分配栈。下次一次管理员 PowerShell 会话可用时，运行：`pwsh -NoProfile -File "C:\Users\ted\Desktop\flowtary\tools\directory_api_bench.ps1" -Entries 10000 -Directories 100 -ActiveSeconds 5 -PauseSeconds 5 -ResumeSeconds 5 -CapturePoolTrace -OutputDirectory "C:\Users\ted\Desktop\flowtary\logs\indexer\directory-api-pooltag-spr-20261105-091000"`。该批次同时包含六项整机指标和分阶段负载；ETW 的缓冲/文件上限及自身跟踪开销须在报告中保留。

为将 UAC 压到一次，`indexer_host_validation_once.ps1` 支持 `-CapturePoolTrace` 并将该开关传给每个 `indexer_host_vhd.ps1` 场景。每个场景先创建/格式化并填充 VHD、跑完只读 provider 探针，再在初建采集前启动有界 pool trace；USN watch/replay 完成后停止并输出该场景 pool-tag 摘要。跟踪窗口明确排除 VHD 创建、格式化和夹具填充。普通权限入口只有一个 `RunAs` 点；提升后的批次串行运行场景，场景子进程继承管理员令牌，不会逐场景弹窗。若 PowerShell 本身已提升则不会调用 RunAs。非破坏性的 `-PlanOnly` 不访问卷、不请求 UAC。

```powershell
pwsh -NoProfile -File "C:\Users\ted\Desktop\flowtary\tools\indexer_host_validation_once.ps1" `
  -ElevatedChild `
  -RunDirectory "C:\Users\ted\Desktop\flowtary\logs\indexer\host-validation-pooltrace-20261105-100000" `
  -Scenarios "file-layout,usn-fallback-resume,directory-link-walk" `
  -ScaleFixtureCount 10000 -WatchSeconds 30 -CapturePoolTrace
```

`-ElevatedChild` 用于从已打开的管理员 PowerShell 直接运行，因而不会调用 `RunAs`。每个成功场景的 ETW 输出位于子目录 `pool-trace.etl`、`pool-summary.txt` 和 start/stop/analysis 日志中。Windows 8 及以后支持多个 SystemTraceProvider session。2026-11-05 本机已有 NT Kernel Logger 与多个系统 logger，xperf 自定义 kernel session 返回 `0x3ec`，而标准 logger 不能安全接管。脚本现使用 Microsoft 文档所示的 `tracelog -start <unique-name> ... -systemlogger` 路径；该修正尚未在提升环境验证。

2026-11-05 11:10 的批次在六指标预检后只触发一次 `RunAs`，Windows 返回“操作已被用户取消”。预检 6 样本、计数器错误 0：提交峰值约 −0.04 MiB、提交率峰值 63.76%；压力 84.42%–84.47%、可用内存最低 4.91 GiB；System 句柄峰值增量 0；分页池峰值增量 0、非分页池 +0.29 MiB；CPU 均值 4.43%；磁盘 busy 1.03%、I/O 约 0.18 MiB/s、页输入峰值 0/秒。结果仅是 UAC 前空闲预检，不能当作索引开销。没有提升 token、VHD、volume access 或 logger；预检文件在 `logs/indexer/host-validation-pooltrace-20261105-111000-preflight/`。没有再次请求 UAC。最终仍须逐 provider 检查完整性、资源守卫、ETW 丢事件状态和 Filter Manager/wcifs 标签后，才可考虑扩大生产 collector。

### 单 UAC 批次运行中的主机资源门与 ETW 启动失败（2026-11-05）

`host-validation-goal-20261105-120500` 的一次 RunAs 成功并依次启动了提升批次。第一个 128 MiB 测试 VHD 完成创建/格式化和 fixture 后，5 秒整机预检发现单个 1 秒页输入峰值 29,498 页/秒（窗口均值 8,145 页/秒），因此在任何 indexer volume probe 前停止；无 indexer/ETW logger 残留，VHD 已卸载。该预检窗口的提交量基线约 48.33 GiB、峰值较首个样本低约 104 MiB；内存压力峰值 88.87%、可用内存最低 3.52 GiB；System 句柄峰值变化 0；分页池约 10.14 GiB、非分页池约 5.74 GiB；整机 CPU 均值 25.87%、磁盘 busy 5.33%、页输入峰值 29,498/秒。没有产生 provider workload 数据。

10 秒普通权限空闲复测计数器错误 0：提交量约 47.51 GiB、窗口峰值 +3.96 MiB；压力 86.48%–86.61%、可用内存最低 4.23 GiB；System 句柄峰值增量 0；分页池 10.12 GiB、非分页池 5.74 GiB；CPU 2.54%、磁盘 busy 2.68%、页输入峰值 31/秒。随后 `host-validation-goal-20261105-121000` 的单 UAC 批次通过预检和 VHD post-fixture 资源门，但 xperf 自定义命名 logger 返回 `0x3ec`，故仍在卷 probe/collect 前退出；VHD 已卸载、没有 ETW logger 残留。第三批 `host-validation-goal-20261105-121600` 的 post-fixture 资源门也通过（峰值页输入 279/秒），但提权后的 logger inventory 显示系统已有 `NT Kernel Logger`、`Circular Kernel Context Logger` 和多个 Windows logger，脚本因无法安全复用而拒绝启动。后续 `121600` 版本改用官方支持的 Tracelog SystemTraceProvider 命名会话；尚未在提权环境验证。前三批均未运行 provider workload，不能作为索引器负载数据。
10 秒普通权限空闲复测计数器错误 0：提交量约 47.51 GiB、窗口峰值 +3.96 MiB；压力 86.48%–86.61%、可用内存最低 4.23 GiB；System 句柄峰值增量 0；分页池 10.12 GiB、非分页池 5.74 GiB；CPU 2.54%、磁盘 busy 2.68%、页输入峰值 31/秒。随后 `host-validation-goal-20261105-121000` 的单 UAC 批次通过预检和 VHD post-fixture 资源门，但 xperf 自定义命名 logger 返回 `0x3ec`，故仍在卷 probe/collect 前退出；VHD 已卸载、没有 ETW logger 残留。第三批 `host-validation-goal-20261105-121600` 的 post-fixture 资源门也通过（峰值页输入 279/秒），但提权后的 logger inventory 显示系统已有 `NT Kernel Logger`、`Circular Kernel Context Logger` 和多个 Windows logger，脚本因无法安全复用而拒绝启动。后续 `121600` 版本改用官方支持的 Tracelog SystemTraceProvider 命名会话；尚未在提权环境验证。前三批均未运行 provider workload，不能作为索引器负载数据。

第四批 `host-validation-goal-20261105-123000` 的 UAC 与管理员 token 均成功，但 VHD harness 的首个 5 秒基线发现高主机 I/O，在创建 VHD 前停止。计数器错误 0；提交量约 47.47 GiB，窗口峰值增长约 173 MiB；压力峰值 88.66%、可用内存最低约 3.58 GiB；System 句柄峰值 +32；分页池约 10.11 GiB、窗口峰值相对起点下降约 10 MiB；非分页池约 5.71 GiB、峰值增长约 9.6 MiB；整机 CPU 均值 24.92%、磁盘 busy 均值 25.09%、页输入均值 15,460/秒、峰值 84,581/秒。该 host I/O 门正确阻止了任何卷访问，仍没有 provider workload 数据。之后脚本新增了同一提升批次内最多一次 5 秒 cooldown retry，且只针对“唯一失败原因是页输入超限”；第二次仍越线或其他门槛失败就停止。这样同一批次不会因短暂页输入 spike 要求重新弹 UAC。此 retry 和新 Tracelog session 尚未在提权运行中验证。

随后 10 秒空闲复测计数器错误 0：提交量约 46.84 GiB、峰值 +64.46 MiB；内存压力 73.41%–73.81%、可用内存最低 8.28 GiB；System 句柄 2,523,545 起、峰值 +40；分页池峰值 9.92 GiB、非分页池峰值 5.71 GiB；CPU 加权均值 13.06%、磁盘 busy 均值 7.39%、读写合计均值约 3.13 MiB/s、页输入均值 232/秒、峰值 8,756/秒。这是空闲/宿主活动参考，不含索引 workload；也说明刚才高页输入窗口不是稳定背景水平。

### Windows Sandbox 真实 NTFS VHD 闭环与整机开销（2026-11-05）

运行目录：`logs/indexer/windows-sandbox-probe/indexer-vm-run-16/`；整机 180 秒采样：`logs/indexer/host-validation-goal-20261105-124100/host-sandbox-attribution-16/`。本轮主机未重启，测试不要求再次提权。之前失败流程的两个具体缺陷已修复：Windows PowerShell 5.1 不支持 `ProcessStartInfo.ArgumentList`，所以外部 DiskPart 包装器在启动前失败；改为兼容的引号参数串并给 DiskPart/fsutil/mountvol 设置有限超时。新批次用独立目录和开始时间校验摘要，避免把策略重启前摘要当成新测试结果。真实卷还暴露出 NTFS 标准内部目录 `$RmMetadata`（MFT 记录 27）在该卷的 `InLostFound` 路径；分类器现在只豁免沿实际父链到达记录号/名称精确匹配的 `$RmMetadata` 或 `$TxfLog` 子树，其他孤立或 `$` 命名路径仍失败关闭。`goz-core` 测试 230 项通过，release 构建成功；完整 Sandbox 验证通过。

真实 VHD 初建生成 274 条 catalog entry，读取 12 个目录批次/264 个目录项，目录句柄峰值 1、结束 0；逐文件硬链接 API 请求数为 0。初建查询返回 3 个 Flowtary `.exe` 路径，耗时 32 µs。USN 恢复后包含 275 条 entry、8 次轮询；夹具重命名、创建、删除和新增硬链接均反映到查询中，普通更新命中 2 条、Flowtary `.exe` 命中 4 条，最终查询 25 µs。测试错误 0，VHD 脱离成功。增量恢复报告里 `directory_batch_requests=0`、单次 USN journal read 1；这是预期的增量路径，不会重跑初建目录枚举。

4 GiB Sandbox 来宾中的索引操作监控计数器没有错误。初建/恢复期间提交量峰值分别约 1.69/1.70 GiB，内存压力峰值约 44.59%/44.88%，System 句柄峰值 4,369/4,348，分页池峰值约 93/102 MiB，非分页池峰值约 221/221 MiB；整机 CPU 样本均值约 0.07%/0.25%，磁盘 busy 样本均值约 26.36%/20.87%，磁盘吞吐样本均值约 0.11/0.45 MiB/s。初建/恢复共 16/51 个监控样本；由于 1 秒采样间隔且操作持续约 6/14 秒，这些数值是短 fixture 的粗粒度整机观测，不能外推为大盘初建时间或逐进程 CPU。

同一轮主机 180 秒窗口覆盖 180 个进程快照、机器计数器错误 0：提交量首样本约 54.72 GB、峰值 58.06 GB（提交率 74.33%）、结束约 53.88 GB；内存压力首样本 78.47%、峰值 89.82%、结束 76.29%，可用内存最低 3.22 GiB；System 句柄首样本 2,531,554、峰值 2,532,705（相对首样本 +1,151）、结束 2,531,789；分页池约 10.06 GiB 起、峰值 10.09 GiB，非分页池约 5.77 GiB 起、峰值 5.84 GiB；整机 CPU 均值 19.01%，磁盘 busy 均值 9.21%、读写吞吐均值约 17.31 MiB/s，页输入峰值 45,426/秒。绝对句柄和内核池仍是该主机异常基线；同一窗口出现 Visual Studio `cl.exe` 和其他宿主进程活动，因此 CPU、磁盘和系统句柄/池的变化不能只归因于索引器。Sandbox 的 `vmmemWindowsSandbox` 私有字节观测峰值约 3.44 GB；由于来宾策略重启前后出现两个先后运行的 vmmem PID，其私有字节增量不可相加。此项是隔离 VM 的整机开销，包含 Windows 来宾，不能当作直接宿主模式下 Flowtary 单进程的内存值。测试结束时 Sandbox VM 与其残留 RemoteSession 已清理，`vmcompute` 仍运行；主机 `LastBootUpTime` 未改变。

本轮证明功能路径在一次性 NTFS VHD 上闭环并提供了六项整机指标；它尚未验证大规模真实磁盘索引，也没有在高资源占用主机上拆分出 Flowtary 单进程对 wcifs/Filter Manager 内核标签的独立贡献。Run 17 后续为每条索引命令增加进程级私有字节、句柄和 CPU 时间采样；整机基线异常时继续只报告窗口增量和背景活动，不把绝对池值归因给索引器。

### 索引器进程级开销采样（2026-11-05）

`indexer-vm-run-17/` 重跑同一夹具，为每条 `flowtary-indexer.exe` 命令增加约 100 ms 间隔的私有字节、工作集、句柄峰值和进程 CPU 时间采样。初始初建命令约 6.9 秒，私有字节峰值 4.64 MiB、工作集峰值 11.23 MiB、句柄峰值 198、CPU 时间 359 ms；增量 USN watch 命令约 15.0 秒（含 8 秒 watch），私有字节峰值 5.20 MiB、工作集 11.79 MiB、句柄峰值 200、CPU 时间 438 ms。一次查询 CLI 进程峰值私有字节约 0.42 MiB、工作集约 2.27 MiB、句柄 33、CPU 时间 16 ms；索引内查询计时为 85 µs，最终查询为 23 µs。进程私有字节峰值来自短时样本，查询子进程启动/退出很快，不能把这些小夹具的值外推到完整磁盘索引。

Run 17 同时完成 180 秒主机采样：提交量首样本 55.21 GB、峰值 59.97 GB（76.78% 提交率）、结束 54.86 GB；压力 77.94% 起、峰值 94.72%、结束 78.35%，可用内存最低 1.67 GiB；System 句柄 2,531,880 起、峰值 2,533,652、结束 2,532,582；分页池 10.18 GiB 起、峰值 10.22 GiB，非分页池 5.78 GiB 起、峰值 5.81 GiB；CPU 平均 22.09%，磁盘 busy 平均 14.72%、读写均值 28.58 MiB/s、峰值 326.78 MiB/s，页输入峰值 82,372/秒，机器计数器错误 0。进程归因中，一个 `vmmemWindowsSandbox` PID 的私有字节增加约 3.19 GB、峰值约 3.44 GB、工作集增加约 3.18 GB；来宾策略重启前后的另一个 PID 分属先后阶段，不相加。该 4 GiB Sandbox 配置在本机背景负载下令压力峰值短暂达到 94.72%，是明显的整机成本；来宾结束并清理后 5 秒复测压力回落至 77.47%、可用内存 7.65 GB，提交量 54.15 GB。由于宿主仍有其他进程活动，不把该整机峰值全归给 Flowtary；将 Sandbox 成本纳入方案评估，并在更大规模评测前先调整来宾内存上限或选用资源更充足的可回滚来宾。

随后对 3 GiB Sandbox 上限作资源门验证（Run 18）。来宾启动后可用内存约 1,314 MiB，低于产品 host-test collector 保留的 1.5 GiB 最低可用内存门，因此在打开测试卷之前停止；报告中的 `native_volume_open_attempts=0`，测试 VHD 最终已脱离。该轮整机 180 秒窗口计数器错误 0，但宿主背景负载同时变化：提交量峰值 60.98 GB、压力峰值 95.16%、可用内存最低 1.53 GiB、System 句柄峰值 2,534,013、分页池峰值 10.22 GiB、非分页池峰值 5.86 GiB、CPU 均值 39.41%、磁盘 busy 均值 18.40%、吞吐均值 24.97 MiB/s、页输入峰值 96,429/秒。`vmmemWindowsSandbox` 私有字节峰值约 3.19 GB，工作集增长约 2.67 GB。来宾策略和 Sandbox VM 已退出，主机采样窗仍包含后台进程；故 3 GiB 配置既未通过产品安全门，也没有显示较低的整机压力，暂不继续降低来宾上限或在此负载下扩大索引规模。

### 10,000 文件 NTFS VHD 采集（2026-11-05）

Run 19 将授权测试夹具扩展到 10,002 个文件和 10,003 个路径名（含一个硬链接），放在 128 MiB 一次性 NTFS VHD。索引初建结果 10,018 条，目录批量读取 32 次/10,008 项，目录句柄峰值 1、结束 0，逐文件硬链接 API 调用 0。故障注入 checkpoint 进程约 6.90 秒、CPU 297 ms；checkpoint 恢复进程约 7.10 秒、CPU 516 ms，私有字节峰值 7.23 MiB、工作集峰值 12.95 MiB、句柄峰值 200。两次时间包含各自的指标预热与冷却，不等同于纯扫描耗时。增量 USN watch 进程约 14.99 秒（含 8 秒监听），私有字节峰值 8.95 MiB、工作集峰值 14.88 MiB、句柄峰值 200、CPU 时间 500 ms；8 次轮询后索引为 10,019 条。查询返回 4 个 Flowtary `.exe` 路径，最终内部查询 25 µs。所有采集计数器错误为 0，VHD 脱离成功。

Sandbox 来宾内初建/增量窗口的整机指标：提交量峰值约 1.71/1.73 GiB，内存压力峰值 46.15%/46.09%，可用内存最低 2.15/2.16 GiB，System 句柄峰值 4,378/4,357，分页池峰值 129.8/138.2 MiB，非分页池峰值 244.9/239.3 MiB，CPU 加权均值约 2.47%/2.99%，磁盘 busy 均值 11.71%/23.29%，读写吞吐均值约 1.10/1.98 MiB/s，页输入峰值 8,316/398 每秒。初建包含故障注入与断点恢复，因此需把两个进程报告合并理解。

同一轮 180 秒宿主采样覆盖 175 个机器样本、机器计数器错误 0：提交量 55.82 GB 起、峰值 60.32 GB、结束 55.90 GB；内存压力 77.15% 起、峰值 92.17%、结束 78.54%，可用内存最低 2.47 GiB；System 句柄 2,533,015 起、峰值 2,535,349、结束 2,534,477；分页池 10.08 GiB 起/10.20 GiB 峰值，非分页池 5.78/5.87 GiB；CPU 均值 21.95%，磁盘 busy 均值 17.27%、读写吞吐均值 23.32 MiB/s、峰值 455.46 MiB/s，页输入峰值 59,763/秒。`vmmemWindowsSandbox` 单 PID 私有字节峰值约 3.44 GB；来宾策略重启前后的 PID 属于不同时段，不累计。整机峰值包含 Sandbox 和背景活动，宿主本身句柄/池基线异常，不能归因成索引器独占开销。VM 及本轮 RemoteSession 清理后，5 秒恢复采样压力回到 76.27%、可用内存 7.50 GiB、提交量 54.93 GB；主机未重启。

该规模证明 NTFS 批量目录遍历保持单目录句柄且能在约 10k 条夹具上建立索引；尚未证明百万级真实卷的初始化曲线。Run 19 的变更在 watch 启动边界附近发生，`changed_polls=0`，没有证明监听后捕获。Run 20 修正时序为等到采样文件出现 `native_idle_wait` 后再修改夹具，结果 `changed_polls=1`，创建/重命名结果 2 条、硬链接路径结果 4 条；但删除路径查询因 JSON 漏传必填 `words` 字段退出，旧脚本未检查该查询退出码，误把该轮标为 completed。脚本现补上 `words: []`、检查所有查询退出码/报告并要求删除路径无命中。Run 20 只作为实时 USN 捕获证据，不能作为删除断言通过。

Run 20 的 174 个宿主机器样本也完整覆盖六项指标、计数器错误 0：提交量 56.73 GB 起、峰值 60.32 GB、结束 55.13 GB；内存压力峰值 92.73%、可用内存最低 2.30 GiB；System 句柄 2,535,086 起、峰值 2,536,217、结束 2,535,282；分页池约 10.13 GiB 起/峰值，非分页池 5.80/5.85 GiB；CPU 均值 21.77%，磁盘 busy 均值 17.64%、吞吐均值 40.96 MiB/s、峰值 536.56 MiB/s，页输入峰值 79,650/秒。Sandbox `vmmemWindowsSandbox` 私有字节峰值约 3.44 GB。Sandbox 及残留会话清理后 5 秒复测压力为 74.12%、可用内存 8.18 GiB、提交量 54.68 GB。主机没有重启；上述峰值包含当时宿主背景进程。

### 10,000 文件 NTFS VHD 闭环复验与宿主恢复（Run 21，2026-11-05）

Run 21 目录为 `logs/indexer/windows-sandbox-probe/indexer-vm-run-21/`，宿主 180 秒采样为 `logs/indexer/host-validation-goal-20261105-124100/host-sandbox-attribution-21/`。一次性 128 MiB NTFS VHD 中有 10,002 个文件、10,003 个路径名。初建 10,018 条 entry，32 次目录批量读取覆盖 10,008 个目录项；目录句柄峰值 1、结束 0，硬链接逐文件 API 请求为 0。断点恢复后 10,019 条 entry，USN watch 8 次轮询并捕获 1 次变更轮询。创建/重命名路径查询命中 2 条，新增硬链接后的 Flowtary `.exe` 查询命中 4 条；已删除文件精确路径命中 0 条且查询未截断。最终查询索引内耗时 27 µs；测试错误 0，VHD 成功脱离。

Sandbox 内索引进程初建约 7.0 秒、CPU 516 ms，私有字节峰值 6.57 MiB、句柄峰值 200；故障注入 checkpoint 进程约 6.93 秒、CPU 391 ms。USN 恢复进程约 15.02 秒（含 8 秒监听），CPU 578 ms、私有字节峰值 8.90 MiB、句柄峰值 200。一次查询进程约 35–41 ms、私有字节峰值约 0.43 MiB、句柄 33。此为 10k 小夹具，不能外推至百万级卷。

同一轮 180 秒宿主窗口 176 个机器观测样本、计数器错误 0：提交量 55.88 GB 起、峰值 59.66 GB、结束 54.96 GB；内存压力峰值 91.51%、可用内存最低 2.68 GiB；System 句柄 2,535,282 起、峰值 2,536,310、结束 2,535,223；分页池约 10.08 GiB 起、10.14 GiB 峰值，非分页池约 5.79 GiB 起、5.87 GiB 峰值；整机 CPU 加权均值 25.56%，磁盘 busy 均值 11.55%、吞吐均值 17.40 MiB/s，页输入峰值 36,361/秒。进程归因显示 Sandbox 来宾对应的 `vmmemWindowsSandbox` 两段 PID 各自曾增长约 3.23 GB、2.13 GB，不能跨 PID 相加；`WindowsSandboxRemoteSession` 私有字节增加约 92 MiB、句柄增加 1,370。宿主窗口同时有其他程序活动，因此整机峰值不能归因给索引器。本机基线仍有约 253 万 System 句柄、约 5.79 GiB 非分页池和约 10.08 GiB 分页池；本次索引进程本身不持有这些系统级绝对值。

测试完成后没有发现 Sandbox worker。RemoteSession PID 14584 在核对时已自行退出；没有重启主机，也没有停止 `vmcompute`。随后 5 秒 metrics-only 恢复采样（不访问卷）结束时：提交量 54.78 GB、压力 74.73%、可用内存 7.99 GiB、System 句柄 2,535,070、分页池 10.13 GiB、非分页池 5.78 GiB、页输入约 1/秒、计数器错误 0。主机 `LastBootUpTime` 仍为 `2026-11-03 20:04:45`。恢复采样数据位于 `host-sandbox-attribution-21-recovery/`。本轮证明 10k NTFS 闭环和监听/删除语义；尚未证明更大真实卷上的资源曲线或直接宿主模式下 Flowtary 对 wcifs/Filter Manager 内核池标签的独立贡献。

### 50,000 文件 NTFS VHD 规模评测（Run 25，2026-11-05）

规模测试先经历三个被守卫正确拦下的配置：Run 22 的 100k fixture 在 1 GiB guest VHD 配置下，索引器来宾可用内存降到约 1.32 GiB，低于 1.5 GiB 预检门，`native_volume_open_attempts=0`；Run 23 的 50k fixture 使用 512 MiB 卷，超过 host-test 仅允许 256 MiB 卷的边界；Run 24 的 50k/256 MiB 卷在打开卷后碰到 host-test 原先 20k entry budget。没有放宽内存门或卷大小限制。Run 25 将 `--host-test-vhd` collector budget 调到 100k entries（索引内存仍限制 64 MiB、layout pages 128、总时间 180 秒），用于 50k fixture；Run 26 又调到 150k entries，以覆盖 100k fixture 的目录和元数据 entry。两轮都保留 64 MiB index cap、128 layout-page cap、180 秒总时限和 256 MiB VHD 上限；非测试 collector 默认仍为 1M entries/512 MiB index。

Run 25 在一次性 256 MiB NTFS VHD 上完成 50,000 个规模 fixture 文件（另有 2 个 Flowtary 版本文件和 1 个硬链接路径）。初建 50,019 条 catalog entry；116 次目录批读取覆盖 50,009 项；目录句柄峰值 1、结束 0；逐文件硬链接 API 请求 0。checkpoint 故障注入/恢复闭环成功。USN 恢复后 50,020 条 entry，8 次轮询中 1 次捕获变化。创建/重命名查询命中 2 条，硬链接查询命中 4 条，删除路径命中 0 条且未截断；最终索引内查询 33 µs。错误 0，VHD 成功脱离。guest 内初建进程约 7.0 秒、CPU 500 ms、私有字节峰值 12.11 MiB、句柄峰值 200；checkpoint 中断进程约 7.12 秒、CPU 547 ms；USN 恢复进程约 14.94 秒（含 8 秒监听）、CPU 422 ms、私有字节峰值 17.05 MiB、句柄峰值 200。

Run 25 宿主整机 180 秒窗口覆盖 175 个机器样本、计数器错误 0：提交量 51.17 GiB 起、55.04 GiB 峰值、51.21 GiB 结束；压力峰值 86.33%、可用内存最低 4.32 GiB；System 句柄 2,535,850 起、2,536,750 峰值、2,535,832 结束；分页池 10.12 GiB 起、10.13 GiB 峰值（相对起点 +10.3 MiB），非分页池 5.80 GiB 起、5.82 GiB 峰值（+25.4 MiB）；整机 CPU 均值 21.43%，磁盘 busy 均值 3.63%、吞吐均值 7.53 MiB/s、峰值 88.39 MiB/s，页输入峰值 3,393/秒。窗口含宿主背景负载。Sandbox `vmmemWindowsSandbox` 私有字节峰值 3.44 GB、工作集峰值 3.41 GB；两个先后 PID 不累计。RemoteSession 单 PID 句柄峰值 2,227、私有字节峰值 634 MiB；VM 退出后只结束了命令行明确指向 Run 25 `.wsb` 的残留 RemoteSession。5 秒恢复采样显示压力 75.25%、可用内存 7.82 GiB；采样期页输入尖峰仍混有宿主背景 I/O。主机未重启，`LastBootUpTime` 前后完全相同。

记录目录：guest `logs/indexer/windows-sandbox-probe/indexer-vm-run-25/`；整机及 Sandbox 进程轻量样本 `logs/indexer/host-validation-goal-20261105-125100/host-sandbox-attribution-25/`；清理 RemoteSession 后复测 `host-sandbox-attribution-25-final-recovery/`。Run 25 证明当前实现可在 50k NTFS fixture 下工作，并给出了六项整机成本数据；Run 26 随后把真实 NTFS fixture 推进到 100k。

### 100,000 文件 NTFS VHD 规模评测（Run 26，2026-11-05）

Run 26 将一次性 Sandbox 来宾上限设为 5 GiB（VHD 仍为受限 256 MiB），以保持来宾 1.5 GiB 可用内存门。初建 100,019 条 catalog entry；220 次目录批读取覆盖 100,009 项；目录句柄峰值 1、结束 0；逐文件硬链接 API 请求 0。断点故障注入/恢复通过；USN 恢复后 100,020 条 entry，8 次轮询捕获 1 次变更；创建/重命名命中 2 条、硬链接命中 4 条、删除路径命中 0 条且未截断；最终查询 34 µs。错误 0，VHD 成功脱离。初建进程约 7.11 秒、CPU 547 ms、私有字节峰值 26.85 MiB、句柄峰值 200；checkpoint 中断进程约 7.29 秒、私有字节峰值 21.39 MiB；USN 恢复约 15.09 秒（含 8 秒监听）、CPU 594 ms、私有字节峰值 26.76 MiB、句柄峰值 200；首次/最终查询分别 84/34 µs。

宿主 180 秒窗口覆盖 175 个机器样本、计数器错误 0：提交量 51.29 GiB 起、55.14 GiB 峰值、50.09 GiB 结束；内存压力峰值 92.15%、可用内存最低 2.48 GiB；System 句柄 2,535,814 起、2,536,711 峰值、2,535,694 结束；分页池约 10.12 GiB 起、峰值增加 102.8 MiB，非分页池 5.80 GiB 起、峰值增加 113.1 MiB；CPU 均值 23.33%，磁盘 busy 均值 14.20%、吞吐均值 20.84 MiB/s、峰值 535.13 MiB/s，页输入峰值 63,311/秒。5 GiB Sandbox 的 `vmmemWindowsSandbox` 私有字节峰值约 4.21 GiB、工作集约 4.18 GiB；两个先后 PID 不累加。RemoteSession 句柄峰值 2,214。整机窗口包含宿主背景活动，且系统内核池绝对基线依然异常，池变化不能单独归因给 Flowtary。

清理本轮 Sandbox 和命令行精确匹配 Run 26 的 RemoteSession 后，5 秒复测压力为 73.87%、可用内存 8.26 GiB、提交量 51.56 GB、System 句柄 2,535,586；机器计数器错误 0。`LastBootUpTime` 仍为 `2026-11-03 20:04:45`。Run 26 数据位于 `logs/indexer/windows-sandbox-probe/indexer-vm-run-26/`、`logs/indexer/host-validation-goal-20261105-124100/host-sandbox-attribution-26/` 和 `host-sandbox-attribution-26-final-recovery/`。这把真实 NTFS 样本从 10k 推进到 100k；仍未覆盖百万级或整盘扫描，也未能按 wcifs/Filter Manager 标签拆分具体内核分配来源。

### Pool tag 并行采样与 100,000 文件复测（Run 27/28，2026-11-05）

为定位整机池变化，增加只读 `NtQuerySystemInformation(SystemPoolTagInformation)` 采样脚本 `tools/indexer_pool_tags.ps1`：每秒记录约 4,119 个 tag 中 `WC*`、`FM*` 和非分页池前 20 大项；不启动 ETW logger、不加载驱动、不需要管理员权限。tag 总和明显小于整机池计数器，且 pool tag 只提供分配标签统计，不含调用栈，因此不能单凭它判断分配者进程或将整机变化归因于 Flowtary。

Run 27 的 100k NTFS 测试通过，但第一版 launcher 将采样器误用 Windows PowerShell 5.1 启动，C# 代码的属性初始化语法不兼容，未获得 pool tag 曲线。该次整机 180 秒样本为 175 个、计数器错误 0：提交量约 51.29 GiB 起、55.14 GiB 峰值、47.96 GiB 结束；压力峰值 90.93%、可用内存最低约 2.87 GiB；System 句柄峰值较扫描期起点约 +2,343、结束 +1,451；分页池与非分页池峰值差值分别约 +35.7/+87.9 MiB。由于 Windows Sandbox 自身提交量变化和宿主其他活动均在窗口内，这些增量不能归因给索引器。Sandbox guest 完成测试；主机启动时间前后相同。Run 27 的 sampler stderr 保留在 `host-sandbox-attribution-27/pool-tags.stderr.log`，该轮 pool-tags 文件为零样本，不用于 tag 结论。

将 launcher 改为调用已安装的 PowerShell 7 后，Run 28 重复同一 100k/256 MiB NTFS Sandbox 场景，并同时采集整机六项指标和 1 秒 pool tag。guest 初建 100,019 条 entry，220 次目录批读取覆盖 100,009 项，目录句柄峰值 1、结束 0；USN 恢复后 100,020 条，捕获变更；创建/重命名、硬链接、删除断言通过；最终查询 49 µs；错误 0。Sandbox 内初建约 7.22 秒，索引进程私有字节峰值约 25.4 MiB、句柄峰值 200。`vmmemWindowsSandbox` 私有字节峰值约 4.21 GiB、工作集 4.17 GiB；RemoteSession 私有字节峰值约 659 MiB、句柄峰值 2,216。

Run 28 host 180 秒窗口覆盖 174 个机器样本、计数器错误 0。提交量约 47.92 GiB 起、53.15 GiB 峰值、48.20 GiB 结束；压力峰值 91.56%、可用内存最低 2.67 GiB；System 句柄 2,537,035 起、2,538,010 峰值、2,537,079 结束；分页池约 10.10 GiB 起、峰值高约 35.5 MiB、结束高约 10.8 MiB；非分页池约 5.77 GiB 起、峰值高约 85.1 MiB、结束高约 1.42 MiB。整机 CPU 均值 29.82%、磁盘 busy 均值 8.61%、平均读写合计约 14.2 MiB/s、页输入峰值约 69,485/秒。5 秒恢复采样压力约 74.6%、可用内存约 8.04 GiB，System 句柄与池指标回到接近窗口起点。主机未重启，`LastBootUpTime` 前后均为 `2026-11-03 20:04:45`；只清理命令行精确匹配 Run 28 `.wsb` 的残留 RemoteSession。

本轮 177 个 pool tag 样本中，`WCsc` 非分页已用约 970.824→970.825 MB，`WCfc` 约 298.278→298.278 MB，`FMwi` 约 323.609→323.609 MB；`FMfc` 约 485.590→485.622 MB，采样最大约 485.732 MB。`FMfc` 分配/释放计数在窗口中分别增加约 3,798/3,631；其他三个标签计数变化很小。观察到这些标签已用字节稳定，但该机制仍只有系统池 tag 汇总，不是逐进程/调用栈归因；整机 System 句柄峰值也有约 +975 的短时波动。当前结果不显示本轮扫描使 `WCsc/WCfc/FMwi` 已用池持续增长，但不足以证实其余池变化来自 Sandbox 或 Flowtary，也不能排除其他驱动和宿主后台活动。

Run 28 数据：guest `logs/indexer/windows-sandbox-probe/indexer-vm-run-28/`；整机与 tag 样本 `logs/indexer/host-validation-goal-20261105-124100/host-sandbox-attribution-28/`；恢复窗口 `host-sandbox-attribution-28-recovery/`；启动/中止门与主机启动时间 `host-sandbox-scale-28-preflight/`。评测仍未覆盖百万级或整盘真实卷，也未获得按调用栈区分 wcifs、Filter Manager 与其他驱动的 ETW 证据。

### 125,000 文件 NTFS 扩展评测（Run 29/30，2026-11-05）

Run 29 首次尝试没有进入索引流程：Sandbox 启动命令传入 125,000，但 `run-indexer-scale.ps1` 的 `FixtureCount` 参数校验仍封顶 100,000，来宾脚本在参数绑定时退出，因此没有写出测试卷或 guest summary。该轮整机窗口包含主机后台活动及 Sandbox 启动，不能当作索引成本数据；未将其用于扫描结论。修正 fixture 参数上限后，Run 30 完成同一 125,000 文件场景。

Run 30 在 256 MiB NTFS VHD 上完成初建 125,019 条 entry；272 次目录批读取覆盖 125,009 项；目录句柄峰值 1、结束 0。USN 恢复后 125,020 条，捕获 1 次变更；错误 0。初建约 7.09 秒，索引进程私有字节峰值 31.0 MiB、句柄峰值 200；最终查询 31 µs（初次 37 µs）。

整机 180 秒窗口覆盖 175 个样本、计数器错误 0：提交量约 47.72 GiB 起、52.65 GiB 峰值、47.76 GiB 结束；压力峰值 89.88%、可用内存最低 3.20 GiB；System 句柄 2,537,504 起、2,538,818 峰值、2,537,938 结束；分页池峰值高 32.3 MiB、结束低 12.2 MiB，非分页池峰值高 84.8 MiB、结束高 9.7 MiB；CPU 均值 19.17%、磁盘 busy 均值 8.19%、平均读写合计约 19.76 MiB/s、页输入峰值约 39,565/秒。`vmmemWindowsSandbox` 私有字节峰值约 4.22 GiB、工作集约 4.17 GiB；RemoteSession 私有字节峰值约 0.59 GiB、句柄峰值 2,215。恢复窗口压力约 73.6%、可用内存约 8.35 GiB；主机启动时间前后相同。

同步取得 178 个 pool tag 快照。`WCsc` 非分页已用 970.99→971.16 MB，`WCfc` 298.36→298.39 MB，`FMwi` 323.67→323.72 MB；`FMfc` 起点约 485.68 MB、峰值 485.87 MB、结束 485.75 MB。整体标签余额变化很小，但 System 句柄峰值短时增加约 1,314、窗口结束比起点多 434；这仍只是全机窗口，不能据此归因于 Flowtary。host 180 秒总窗口仍有明显 Sandbox VM 和后台活动成本，需结合 guest 私有内存、恢复读数及标签曲线解读。

Run 30 的 guest、整机、pool tag、恢复数据分别位于 `logs/indexer/windows-sandbox-probe/indexer-vm-run-30/`、`logs/indexer/host-validation-goal-20261105-124100/host-sandbox-attribution-30/` 与 `host-sandbox-attribution-30-recovery/`；主机保护门和 boot-time 记录位于 `host-sandbox-scale-30-preflight/`。

### 150,000 文件 NTFS 上限扩展（Run 31，2026-11-05）

在 host-test 专用 collector 中将 `max_entries` 调到 200,000，以容纳 150,000 个文件及 NTFS/目录元数据 entry；索引内存 64 MiB、布局页 128、采集总时限 180 秒和测试 VHD 最大 256 MiB 均保持不变。release 构建成功，Sandbox 仍只通过显式 `--host-test-vhd` 接触一次性测试 VHD。

Run 31 完成 150,000 文件规模：初建 150,019 条 entry；324 次目录批读取覆盖 150,009 项；目录句柄峰值 1、结束 0；USN 恢复后 150,020 条并捕获 1 次变化；最终查询 33 µs；错误 0。初建约 7.18 秒，索引进程私有字节峰值 37.6 MiB、句柄峰值 200。

整机 180 秒窗口 175 个样本、计数器错误 0：提交量约 47.86 GiB 起、53.72 GiB 峰值、47.72 GiB 结束；压力峰值 91.09%、可用内存最低 2.82 GiB；System 句柄 2,537,977 起、2,539,079 峰值、2,538,071 结束；分页池峰值变化 +7.5 MiB、结束 −42.3 MiB，非分页池峰值 +24.3 MiB、结束 −11.5 MiB；CPU 均值 24.12%、磁盘 busy 均值 9.72%、读写合计均值约 21.17 MiB/s、页输入峰值约 74,821/秒。`vmmemWindowsSandbox` 私有字节峰值 4.21 GiB、工作集 4.17 GiB；RemoteSession 私有字节峰值 0.66 GiB、句柄峰值 2,246。恢复采样压力约 70.0%、可用内存约 9.50 GiB；主机 boot time 前后相同，Run 31 Sandbox 进程均已退出。

178 个同步 pool-tag 快照显示 `WCsc` 非分页用量约 971.156→971.177 MB（峰值 +20 KiB），`WCfc` 298.388→298.393 MB（峰值 +4.5 KiB），`FMwi` 323.720→323.726 MB（峰值 +6.5 KiB）；`FMfc` 约 485.832→485.814 MB（峰值 +119 KiB，结束低于起点）。这次整机 System 句柄峰值短时增加约 1,102、结束 +94，不能归因到某个驱动或 Flowtary。150k 测试把当前受限 VHD 实测规模推进到 150k，但不等价于百万级或用户真实卷的整盘扫描；host 标签统计仍不含调用栈，Sandbox 虚拟机本身约占 4.21 GiB 提交/私有内存成本。

Run 31 数据位于 guest `logs/indexer/windows-sandbox-probe/indexer-vm-run-31/`、整机与 pool tag `logs/indexer/host-validation-goal-20261105-124100/host-sandbox-attribution-31/`、恢复窗口 `host-sandbox-attribution-31-recovery/`、门控状态 `host-sandbox-scale-31-preflight/`。

### 150,000 文件同卷重复初扫趋势（Run 32，2026-11-05）

为了检查重复初扫是否产生逐轮系统资源累积，Run 32 在同一 256 MiB NTFS VHD、同一 Sandbox 内，先完成一次 checkpoint/resume 初建，然后用独立临时快照顺序执行 5 次全量初扫（不并行）；每轮都验证 150,019 条 entry、目录批读取和句柄闭合。新增 `RepeatInitialBuilds` 参数默认 0、此次为 5；同时把 phase transition 的 UTC 时间写入 `phase-history.jsonl`，以便将每轮 guest 阶段与宿主 1 秒样本对齐。整机与 pool-tag 采样窗口延至 300 秒以覆盖重扫和停扫回落；内核采集约束仍是 64 MiB、128 layout pages、180 秒，主机中止门未变。

五轮初扫分别耗时 7.59、7.60、7.58、7.78、7.72 秒；索引进程峰值私有字节 37.0–39.8 MiB、峰值句柄每轮均为 200；活动目录句柄每轮峰值 1、结束 0。同步 1 秒机器样本中，每轮边界 System 句柄变化 −3、+4、−4、−18、+7，未呈逐轮增加；pool tag 的 `WCsc/WCfc/FMwi` 每轮余额变化为 0 或数 KiB，`FMfc` 平稳或略降。复扫后 USN 恢复条数 150,022、捕获 2 次变化、查询 30 µs；guest 错误 0。

300 秒整机窗口覆盖 292 个样本、计数器错误 0：提交量约 47.71 GiB 起、53.63 GiB 峰值、47.50 GiB 结束；压力峰值 91.58%、可用内存最低 2.66 GiB；System 句柄 2,537,922 起、2,538,852 峰值、2,537,986 结束；分页池峰值变化 +14.7 MiB、结束 +2.2 MiB，非分页池峰值 +26.2 MiB、结束 −3.2 MiB。整窗 CPU 均值 13.88%、磁盘 busy 均值 3.96%、平均读写合计 9.61 MiB/s；整窗均值含扫描结束后的冷却时段。`vmmemWindowsSandbox` 私有字节峰值 5.22 GiB、工作集 5.15 GiB；RemoteSession 私有字节峰值 0.64 GiB、句柄峰值 2,211。恢复采样压力 68.4%、可用内存约 9.98 GiB，System 句柄比测试前少 19，内核池回到接近基线；主机启动时间未改变。

全窗口 pool tag 余额：`WCsc` 峰值较起点约 +12 KiB，`WCfc` +4 KiB，`FMwi` +4 KiB，`FMfc` 峰值 +101 KiB、结束 −33 KiB。重复第 4 轮的 8 秒窗口另有一次约 +1.03 GiB 提交量峰值及约 66 MiB/s 平均读写合计；它没有同步体现为目标四个 tag 的余额增长，而且采样不能识别分配者，因此作为未归因的整机/虚拟机瞬时峰值保留，不归给索引器。Sandbox 私有字节峰值也高于单轮测试；这项 VM 成本必须从产品实际运行成本中单独识别，不能把 Sandbox 负载曲线直接等同 Flowtary 自身成本。

Run 32 输出：guest `logs/indexer/windows-sandbox-probe/indexer-vm-run-32/`（含 phase-history 与每轮报告）；整机和 1 秒 pool tag `logs/indexer/host-validation-goal-20261105-124100/host-sandbox-attribution-32/`；恢复窗口 `host-sandbox-attribution-32-recovery/`；保护门/boot time `host-sandbox-scale-32-preflight/`。目前这些重复扫描证据未显示 System 句柄或目标 wcifs/Filter Manager tag 随 5 轮初扫线性累积；它不能排除其他内核池标签、百万级真实卷形状或长时运行下的问题。

## 2026-11-15：重启后无卷只读基线——异常绝对基线已消失，仅剩非分页池一项越线

运行目录 `logs/indexer/host-baseline-20261115/`。只运行 `metrics`（8–10 秒、不开卷、不提权、不枚举），原始样本 `samples.jsonl`/`samples2.jsonl`，报告 `report2.json`。计数器错误 0。

与 11-04/05 各节记录的异常绝对基线（System 句柄约 252 万、分页池约 9.8 GiB、非分页池约 5.7 GiB）相比，本机已重启，这些异常值全部消失：

- System 句柄（`\Process(System)\Handle Count`）：**28,267**（门槛 ≤100,000，通过）。
- 分页池：**3.06 GiB**（门槛 ≤4 GiB，通过）。
- 非分页池：**2.22 GiB**（门槛 ≤2 GiB，**唯一越线项**，超约 0.22 GiB）。
- 可用物理内存最低 **9.05 GiB**（≥2 GiB，通过）；提交率峰值 **0.565**（≤0.8，通过）；内存压力峰值 **0.714**（≤0.90，通过）。

`native_host_violations` 现仅返回 `["nonpaged pool baseline exceeds 2 GiB"]`。即采集硬门槛从「三项绝对基线灾难性越线」收敛到「仅非分页池略超」；是否 transient 需在更空闲窗口复测确认。本轮没有打开卷、没有申请 UAC、没有运行采集，不构成对真实 C 卷采集的放行；真实卷完整性验证、经确认的有界采集与快照发布仍是前置条件。门槛本身不放宽。

## 2026-11-15：纯 MFT 硬链接通道——C: 权限探针与真卷 VHD 验证

为对标 Everything（MFT/USN、零目录遍历）把硬链接补名从逐文件 FindFirstFileNameW 改为纯 MFT 的 `FSCTL_GET_NTFS_FILE_RECORD`（base+extension 名称数量审计）。详见 `docs/SEARCH_ENGINE_SELECTION.md` 同日节。

- C: 权限探针（提权，`logs/indexer/host-C-mft-privilege-fix-20261115-01/`，二进制 SHA256 ED55BCC2…）：在 `for_test` 开卷前补调 `enable_volume_privileges()` 后，`$MFT` 流打开仍 error 5、OpenFileById 仍 error 5、QUERY_FILE_LAYOUT 仍 error 50；`FSCTL_GET_NTFS_FILE_RECORD` 成功读出含 2 个硬链接名的 FILE 记录。结论：error 5 非缺备份权限（该判断已撤回），C: 第三方安全 minifilter（sysdiag/lrtp/LnvMSRIO 等）疑似拦截批量 $MFT/QUERY_FILE_LAYOUT；逐 FRN MFT 记录读可用。
- 真卷 VHD 验证（`logs/indexer/host-vhd-pure-mft-20261115-01/`，`-VerifyInitialResume` 生产纯 MFT 路径）：exit 0、completed；初建查 flowtary exe 返回 3 名（含跨目录硬链接 alias），断点续建/USN 监听/增删改/回放/硬链接审计全过。io：hardlink_name_requests=0、file_record_requests=259、directory_handles_opened=0、directory_batch_requests=0。Rust 离线 48 项通过。
- 本轮仅打开授权测试 VHD 与 C: 单记录探针，未做 C 卷全量采集；native_host_violations 门槛未放宽（非分页池仍略超）。
