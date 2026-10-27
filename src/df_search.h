// ============================================================
// df_search.h — d/f 搜索后端公共接口（不建全盘索引）
//   ① Windows Search：查询系统自带的 WSearch 索引服务（SystemIndex），
//      覆盖范围由系统「索引选项」决定（默认「经典」= 开始菜单 + 用户库，
//      可在设置里把常用目录加进去）。
//   ② fd（https://github.com/sharkdp/fd）：每次查询现场并行遍历固定盘，
//      无状态进程、查完即退，空闲内存为零。
//   主程序（src/main.cpp）与冒烟工具（tools/wsquery.cpp）共用本接口。
// ============================================================
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace df {

struct Hit {
    std::wstring path;      // 完整路径（含文件名；盘根形如 C:\）
    bool isFolder = false;
};

struct Query {
    bool isFile = true;                  // f=只搜文件 / d=只搜文件夹
    std::vector<std::wstring> words;     // 正向词（"短语" 已合成一个词、去引号）
    std::vector<std::wstring> excludes;  // -词（小写）：命中即丢弃（子串，大小写不敏感）
};

// ---- Windows Search 后端 ----
// 轻量探测：provider 可加载且能执行一条琐碎查询（结果进程内不缓存，调用方自行缓存）。
bool WindowsSearchAvailable();
// 诊断：跑一条琐碎查询，返回各阶段 HRESULT（"CLSID=0x... CREATE=0x... …"），wsquery --check 用。
std::wstring DiagProbe();
// 按名字匹配（pathMode=false，System.FileName）或整条路径兜底（true，System.ItemUrl）。
// 返回命中数；失败返回 -1（WSearch 未运行 / provider 不可用等），此时 out 置空。
int WindowsSearchQuery(const Query& q, bool pathMode, std::vector<Hit>& out);

// ---- fd 后端 ----
// fd.exe 定位：flowtary.exe 同目录优先，其次 PATH。找不到返回空串。
std::wstring FindFdExe();
// fd 单次查询预算（毫秒）：读 FT_DF_FD_TIMEOUT_MS（0=不限时），缺省 8000。
DWORD DefaultFdDeadlineMs();
// 用户常用目录根（桌面/文档/下载/图片/音乐/视频）与全部固定盘根。
// fd 的共享遍历队列没有根优先级——大根会把小根的结果淹没到几十秒之后
// （实测：Desktop 单根 1.6s 命中，加 C:\ 根后 26s+），所以分两组、由调用方分阶段扫。
std::vector<std::wstring> UserDirsRoots();
std::vector<std::wstring> DriveRoots();
// 现查固定盘。skipHidden=false 时加 -H 连点开头文件/目录一起返回
//（点开头过滤与用户排除列表由调用方客户端过滤，与 Everything 结果同规则）。
// deadlineMs：本次遍历的时间预算（0=不限时）。实时防护开启的机器上全盘遍历可达
// 分钟级，超时即终止 fd 并保留已读到的部分结果（*timedOut 置 true，调用方据此
// 跳过路径兜底，保证总耗时不超过预算）。onBatch 非空时结果按批次增量投递
//（流式上屏），out 仍收全量。rootsOverride 非空时用给定根（否则 用户目录+全盘）。
// 返回命中数；fd 启动失败返回 -1。
int FdQuery(const std::wstring& fdExe, const Query& q, bool pathMode, bool skipHidden,
            std::vector<Hit>& out, DWORD deadlineMs, bool* timedOut,
            const std::function<void(std::vector<Hit>&&)>& onBatch = {},
            const std::vector<std::wstring>& rootsOverride = {});
// 终止所有在途 fd 进程（新一轮查询开始 / 窗口隐藏时调用，防止旧查询继续占 CPU）。
void CancelAll();

// ---- 内存索引（用户常用目录，毫秒级子串搜索）----
// 范围仅 UserDirsRoots()（桌面/文档/下载/图片/音乐/视频）+ extraRoots（注册表
// IndexDirs 配置的项目目录等）——不是全盘索引。
// 用 fd 全列建库（秒级），常驻内存（条目上限 FT_DF_MEMINDEX_MAX，默认 300000）；
// 目录变更经 ReadDirectoryChangesW 监听：增/删/改名事件即时入 RAM delta 表
// （查询时与池合并，新条目秒级可搜），防抖后按根增量重建折叠进池。
// 建库期间 MemIndexReady()=false，调度链自动落到 fd/WS，就绪后自动接管。
// 拼音：守护进程（--serve）内启用，汉字名字附存全拼，词条可按「名字 / 全拼子串 /
// 首字母子序列（≥2字符）」命中；exe 同目录无 dict/ 时自动降级为纯名字匹配。
void MemIndexStart(const std::vector<std::wstring>& extraRoots = {});   // 幂等：后台建库+监听
bool MemIndexReady();            // 首次建库是否完成
size_t MemIndexSize();           // 当前条目数（诊断用）
// 名字模式（pathMode=false，按文件名子串）或整条路径模式（true）。返回命中数。
int MemIndexSearch(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows);

// ---- 磁盘索引（全盘覆盖，索引文件化 + mmap 扫描，进程私有内存近零）----
// fd 全盘遍历一次（空闲优先级，数百秒）→ 小写化 UTF-8 路径 blob（数百 MB 磁盘文件）
// → MapViewOfFile 只读映射，查询 = 首字节 memchr 全 blob 扫描（暖 20–40ms）。
// 新鲜度：每盘根 ReadDirectoryChangesW → RAM 内小 delta 表（增/删），查询时合并；
// delta 超 5 万条自动后台重建。blob 可被系统换出，不计入进程私有内存。
// FT_DF_DISKINDEX_MAX 限条目数（默认 8000000）、FT_DF_DISKINDEX_OFF=1 关闭。
// 缓存格式 v3（FTDISKIDX3）：行 = "D|F<小写路径>[|<basename 全拼>]"——拼音开关开启且
// 词典可用时建库写入，扫描按首个 '|' 切分（Windows 路径不含 '|'）；拼音开启但缓存
// 是无拼音版（PY0）时后台重建一次补上。
void DiskIndexStart();           // 幂等：映射已有索引；缺失/过期则后台建库
bool DiskIndexReady();
int  DiskIndexSearch(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows);
size_t DiskIndexRecords();       // 条目数（诊断用）

// ---- 搜索守护进程（Flowtary Search 服务）----
// 内存索引 + 磁盘索引都驻留在 flowtary-search.exe --serve 进程内：
// flowtary.exe 回归纯 UI（私有内存 ~25MB），索引内存（池 + blob 映射）记在
// flowtary-search.exe 名下，任务管理器与 Everything 的对比一目了然。
// 协议：TCP 127.0.0.1:47771（FT_DF_DAEMON_PORT 可覆盖），行式 UTF-8：
//   C: "Q <isFile> <pathMode> <maxRows> <nwords> <nexcl>\n" + 词/排除词各一行
//   S: "N <count>\n" + count 行 "<D|F><path>\n" + "END\n"
//   C: "STAT\n" → S: "M <ready> <size>\nD <ready> <records>\nEND\n"
// 守护进程单实例（互斥体），空闲 30 分钟自动退出（磁盘缓存仍在，重开秒级加载）；
// 崩溃不影响 flowtary（查询失败自动落到 fd/WS 层，下次查询自动重拉守护进程）。
// 索引档位（设置「常规」页，内存↔速度权衡）：守护进程自读注册表
//   HKCU\Software\Flowtary\DfIndexTier（0=极简 1=轻量 2=均衡 3=极速）与
//   DfPinyin（拼音开关）——启动时应用条目上限与拼音开关；运行中任一查询连接
//   或 60s 轮询发现变更即退出（回 BYE），主程序下轮查询自动按新档重拉。
//   极简档守护进程启动即退（不驻留），主程序不拉起、查询走 fd/WS 实时兜底。
// 守护进程服务主循环（flowtary-search --serve 进程入口调用，阻塞直至空闲退出）
int SearchDaemonServe(const std::vector<std::wstring>& extraRoots);
void SearchDaemonStart();        // 幂等：确保守护进程在跑（不在线则拉起，最多等 ~3s）
bool SearchDaemonQuery(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows);
bool SearchDaemonStat(int& memReady, int& memSize, int& diskReady, int& diskRecords,
                      int* diskError = nullptr, int* diskBuilding = nullptr);

}  // namespace df
