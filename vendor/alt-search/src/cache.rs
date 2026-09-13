use std::path::Path;
use std::time::SystemTime;

use jwalk::WalkDir;
use serde::{Deserialize, Serialize};

use ahash::AHashMap;
use memchr::memmem;

/// 缓存格式版本:只有 Cache 序列化结构真正变化时才 +1(旧缓存解码失败或
/// 版本不符都会整体重建)。搜索逻辑/守护进程等纯代码改动不得动它,
/// 保证改代码后既有索引照常复用。
pub const CACHE_VERSION: u32 = 2;

#[derive(Debug, Serialize, Deserialize)]
pub struct FileEntry {
    pub name: String,
    pub extension: Option<String>,
    pub size: u64,
    pub modified: u64,
    pub created: u64,
    pub is_dir: bool,
}

/// path → FileEntry 索引缓存。
///
/// Flowtary fork:名字索引由原「AHashMap 逐名 contains」改为「小写名字
/// 连续缓冲区(NUL 分隔)+ 偏移表」,查询走 memchr SIMD 子串扫描——
/// 160 万条目实测命中 ~96ms → ~7ms、未命中 ~228ms → ~4ms,内存也更省
/// (连续 43MB 替代上百万个小字符串分配)。删除采用惰性压实:命中时经
/// entries 校验活性,死条目超阈值后整体重建。
#[derive(Debug, Serialize, Deserialize)]
pub struct Cache {
    /// 缓存格式版本;加载时与 CACHE_VERSION 不符则整体重建
    pub version: u32,

    /// 索引根目录;守护进程加载缓存后与本次启动参数不一致则整体重建
    #[serde(default)]
    pub roots: Vec<String>,

    entries: AHashMap<String, FileEntry>,

    // —— 名字索引(不入缓存文件,load/build 后全量重建,watcher 增量维护)——
    #[serde(skip)]
    name_buf: Vec<u8>,         // 所有小写名字,NUL 分隔
    #[serde(skip)]
    name_starts: Vec<u32>,     // idx → 名字起始偏移;len = 条数 + 1(末尾哨兵)
    #[serde(skip)]
    dir_flags: Vec<bool>,      // idx → 是否文件夹
    #[serde(skip)]
    paths_by_idx: Vec<String>, // idx → 完整路径
    #[serde(skip)]
    dead_count: usize,         // 已删除、尚未压实的条数
}

fn is_boundary(b: u8) -> bool {
    matches!(b, b' ' | b'-' | b'_' | b'.' | b'(' | b')' | b'[' | b']' | b'@' | b'#' | b'~')
}

/// 有界 top-N 收集:满额时替换其中最差者(线性扫,keep 很小,均摊可忽略)
fn upsert_best(best: &mut Vec<(u8, u32)>, keep: usize, score: u8, idx: u32) {
    if best.len() < keep {
        best.push((score, idx));
        return;
    }
    let mut worst = 0;
    for i in 1..best.len() {
        if (best[i].0, best[i].1) > (best[worst].0, best[worst].1) {
            worst = i;
        }
    }
    if (score, idx) < (best[worst].0, best[worst].1) {
        best[worst] = (score, idx);
    }
}

impl Cache {
    pub fn new() -> Cache {
        Cache {
            version: CACHE_VERSION,
            roots: Vec::new(),
            entries: AHashMap::new(),
            name_buf: Vec::new(),
            name_starts: vec![0], // 哨兵:不变式 starts.len() == paths.len() + 1
            dir_flags: Vec::new(),
            paths_by_idx: Vec::new(),
            dead_count: 0,
        }
    }

    pub fn build(&mut self, root: &Path) -> std::io::Result<usize> {
        self.build_par(root, jwalk::Parallelism::RayonDefaultPool { busy_timeout: std::time::Duration::from_secs(10) })
    }

    /// 流式入库(边遍历边插表,不再先 collect 整棵树,峰值内存减半以上);
    /// threads 限制遍历并行度,给宿主机留 CPU(默认全池会吃满所有核)。
    pub fn build_par(&mut self, root: &Path, par: jwalk::Parallelism) -> std::io::Result<usize> {
        for entry in WalkDir::new(root)
            .min_depth(1)
            .max_depth(usize::MAX)
            .parallelism(par)
            .into_iter()
            .filter_map(|e| e.ok())
        {
            let metadata = match entry.metadata() {
                Ok(m) => m,
                Err(_) => continue,
            };
            let file_entry = FileEntry {
                name: entry.file_name().to_string_lossy().to_string(),
                extension: entry.path().extension().map(|e| e.to_string_lossy().to_string()),
                size: metadata.len(),
                modified: metadata.modified()
                    .ok()
                    .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
                    .map(|d| d.as_secs())
                    .unwrap_or(0),
                created: metadata.created()
                    .ok()
                    .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
                    .map(|d| d.as_secs())
                    .unwrap_or(0),
                is_dir: entry.file_type().is_dir(),
            };
            self.entries.insert(entry.path().to_string_lossy().to_string(), file_entry);
        }
        self.rebuild_arena();

        Ok(self.entries.len())
    }

    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        let bytes = postcard::to_allocvec(self)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e))?;
        let compressed = zstd::encode_all(bytes.as_slice(), 3)?;
        std::fs::write(path, compressed)?;
        Ok(())
    }

    pub fn load(path: &Path) -> std::io::Result<Cache> {
        let bytes = std::fs::read(path)?;
        let decompressed = zstd::decode_all(bytes.as_slice())?;

        let mut cache = postcard::from_bytes::<Cache>(&decompressed)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e))?;
        cache.rebuild_arena();
        Ok(cache)
    }

    // ---- 名字索引维护 ----

    /// 全量重建名字索引(load/build 后);1.6M 条约几十 ms。
    /// 注意:这里直接内联 append 逻辑,边遍历 entries 边写其余字段
    /// (字段级不相交借用,不能经过 &mut self 的辅助方法)。
    fn rebuild_arena(&mut self) {
        self.name_buf.clear();
        self.name_starts.clear();
        self.dir_flags.clear();
        self.paths_by_idx.clear();
        self.name_starts.push(0);
        for (path, e) in &self.entries {
            let lower = e.name.to_lowercase();
            self.name_buf.extend_from_slice(lower.as_bytes());
            self.name_buf.push(0);
            self.name_starts.push(self.name_buf.len() as u32);
            self.dir_flags.push(e.is_dir);
            self.paths_by_idx.push(path.clone());
        }
        self.dead_count = 0;
    }

    fn arena_append(&mut self, path: &str, e: &FileEntry) {
        let lower = e.name.to_lowercase();
        self.name_buf.extend_from_slice(lower.as_bytes());
        self.name_buf.push(0);
        self.name_starts.push(self.name_buf.len() as u32);
        self.dir_flags.push(e.is_dir);
        self.paths_by_idx.push(path.to_string());
    }

    /// 名字缓冲区里的命中位置 → (idx, entry)。
    /// 已删除的 idx(entries 中已无此路径)返回 None,实现惰性删除。
    fn entry_at(&self, abs: usize) -> Option<(usize, &FileEntry)> {
        let idx = self.name_starts.partition_point(|&s| (s as usize) <= abs).checked_sub(1)?;
        if idx >= self.paths_by_idx.len() {
            return None;
        }
        let p = &self.paths_by_idx[idx];
        self.entries.get(p).map(|e| (idx, e))
    }

    // ---- 查询 ----

    /// SIMD 子串扫描,返回全部命中(无序)。search.rs 旧接口使用。
    pub fn search_by_name(&self, name: &str) -> Vec<&FileEntry> {
        let ql = name.to_lowercase();
        if ql.is_empty() {
            return self.entries.values().collect();
        }
        let finder = memmem::Finder::new(ql.as_bytes());
        let mut out = Vec::new();
        let mut pos = 0usize;
        while let Some(h) = finder.find(&self.name_buf[pos..]) {
            let abs = pos + h;
            pos = abs + 1;
            if let Some((_, e)) = self.entry_at(abs) {
                out.push(e);
            }
        }
        out
    }

    /// top-N 搜索:名字需包含全部分词;排序为 前缀命中 < 词边界命中 < 普通包含,
    /// 同分按索引序;可按 is_dir 过滤。返回 (是否文件夹, 完整路径)。
    pub fn search_top(&self, terms: &[String], max: usize, is_dir: Option<bool>) -> Vec<(bool, String)> {
        if terms.is_empty() || max == 0 {
            return Vec::new();
        }
        let lower: Vec<String> = terms.iter().map(|t| t.to_lowercase()).collect();
        let first = memmem::Finder::new(lower[0].as_bytes());
        let rest: Vec<memmem::Finder> = lower[1..]
            .iter()
            .map(|t| memmem::Finder::new(t.as_bytes()))
            .collect();

        let keep = (max.saturating_mul(4)).max(64);
        let mut best: Vec<(u8, u32)> = Vec::new();
        let mut pos = 0usize;
        let mut last_idx = usize::MAX;
        while let Some(h) = first.find(&self.name_buf[pos..]) {
            let abs = pos + h;
            pos = abs + 1;
            let Some((idx, e)) = self.entry_at(abs) else { continue };
            if idx == last_idx {
                continue; // 同一名字内的二次命中
            }
            last_idx = idx;
            if let Some(want) = is_dir {
                if e.is_dir != want {
                    continue;
                }
            }
            let s = self.name_starts[idx] as usize;
            let end = self.name_starts[idx + 1] as usize;
            let name = &self.name_buf[s..end];
            if rest.iter().any(|f| f.find(name).is_none()) {
                continue;
            }
            let score = if abs == s {
                0u8
            } else if is_boundary(self.name_buf[abs - 1]) {
                1u8
            } else {
                2u8
            };
            upsert_best(&mut best, keep, score, idx as u32);
        }

        best.sort_by_key(|&(sc, i)| (sc, i));
        best.into_iter()
            .take(max)
            .filter_map(|(_, i)| {
                let p = &self.paths_by_idx[i as usize];
                self.entries.get(p).map(|e| (e.is_dir, p.clone()))
            })
            .collect()
    }

    fn entry_from_path(path: &Path) -> Option<(String, FileEntry)> {
        let metadata = std::fs::metadata(path).ok()?;
        let file_entry = FileEntry {
            name: path.file_name()?.to_string_lossy().to_string(),
            extension: path.extension().map(|e| e.to_string_lossy().to_string()),
            size: metadata.len(),
            modified: metadata.modified()
                .ok()
                .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
                .map(|d| d.as_secs())
                .unwrap_or(0),
            created: metadata.created()
                .ok()
                .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
                .map(|d| d.as_secs())
                .unwrap_or(0),
            is_dir: metadata.is_dir(),
        };
        Some((path.to_string_lossy().to_string(), file_entry))
    }

    pub fn add_entry(&mut self, path: &Path) {
        if let Some((key, entry)) = Self::entry_from_path(path) {
            if self.entries.contains_key(&key) {
                // 已在索引:原地更新元数据;名字没变,名字索引不动
                self.entries.insert(key, entry);
                return;
            }
            self.arena_append(&key, &entry);
            self.entries.insert(key, entry);
        }
    }

    pub fn remove_entry(&mut self, path: &Path) {
        let key = path.to_string_lossy().to_string();
        if self.entries.remove(&key).is_some() {
            self.dead_count += 1;
            // 死条目过多:下次查询前压实,避免名字缓冲区无限膨胀
            if self.dead_count > 64 && self.dead_count * 4 > self.entries.len() {
                self.rebuild_arena();
            }
        }
    }

    pub fn update_entry(&mut self, path: &Path) {
        self.remove_entry(path);
        self.add_entry(path);
    }

    pub fn iter(&self) -> impl Iterator<Item = &FileEntry> {
        self.entries.values()
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }
}
