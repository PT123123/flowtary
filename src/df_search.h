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
// 目录变更经 ReadDirectoryChangesW 监听、防抖后按根增量重建。
// 建库期间 MemIndexReady()=false，调度链自动落到 fd/WS，就绪后自动接管。
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
void DiskIndexStart();           // 幂等：映射已有索引；缺失/过期则后台建库
bool DiskIndexReady();
int  DiskIndexSearch(const Query& q, bool pathMode, std::vector<Hit>& out, DWORD maxRows);
size_t DiskIndexRecords();       // 条目数（诊断用）

}  // namespace df
