use std::path::Path;
use std::time::SystemTime;
use jwalk::{WalkDir};
use serde::{Serialize, Deserialize};
use rayon::*;
use ahash::AHashMap;

#[derive(Debug, Serialize, Deserialize)]
pub struct FileEntry {
    pub name: String,
    pub extension: Option<String>,
    pub size: u64,
    pub modified: u64,
    pub created: u64,
    pub is_dir: bool,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Cache {
    entries: AHashMap<String, FileEntry>,

    #[serde(skip)]
    name_index: AHashMap<String, Vec<String>>,
}

impl Cache {
    pub fn new() -> Cache {
        let entries = AHashMap::new();
        let name_index = AHashMap::new();
        Cache { entries, name_index }
    }

    pub fn build(&mut self, root: &Path) -> std::io::Result<usize> {
        let file_entries: Vec<(String, FileEntry)> = WalkDir::new(root)
            .min_depth(1)
            .max_depth(usize::MAX)
            .into_iter()
            .filter_map(|e| e.ok())
            .filter_map(|entry| {
                let metadata = entry.metadata().ok()?;
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
                Some((entry.path().to_string_lossy().to_string(), file_entry))
            })
            .collect();

        for (key, value) in file_entries {
            self.name_index
                .entry(value.name.to_lowercase())
                .or_insert_with(Vec::new)
                .push(key.clone());

            self.entries.insert(key, value);
        }

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

        let pairs: Vec<(String, String)> = cache.entries
            .iter()
            .map(|(path, entry)| (entry.name.to_lowercase(), path.clone()))
            .collect();

        for (name, path) in pairs {
            cache.name_index
                .entry(name)
                .or_insert_with(Vec::new)
                .push(path);
        }

        Ok(cache)
    }

    pub fn search_by_name(&self, name: &str) -> impl Iterator<Item=&FileEntry> {
        let name_lower = name.to_lowercase();
        self.name_index
            .iter()
            .filter(move |(k, _)| k.contains(&name_lower))
            .flat_map(|(_, paths)| paths.iter())
            .filter_map(|path| self.entries.get(path))
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
            self.name_index
                .entry(entry.name.to_lowercase())
                .or_insert_with(Vec::new)
                .push(key.clone());
            self.entries.insert(key, entry);
        }
    }

    pub fn remove_entry(&mut self, path: &Path) {
        let key = path.to_string_lossy().to_string();
        if let Some(entry) = self.entries.remove(&key) {
            let name_lower = entry.name.to_lowercase();
            if let Some(paths) = self.name_index.get_mut(&name_lower) {
                paths.retain(|p| p != &key);
                if paths.is_empty() {
                    self.name_index.remove(&name_lower);
                }
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
