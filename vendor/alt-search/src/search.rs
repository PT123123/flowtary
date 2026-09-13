use crate::cache::{Cache, FileEntry};

pub struct Query {
    pub name_contains: Option<String>,
    pub extension: Option<String>,
    pub min_size: Option<u64>,
    pub max_size: Option<u64>,
    pub modified_after: Option<u64>,
    pub is_dir: Option<bool>,
}

impl Query {
    pub fn new() -> Self {
        Query {
            name_contains: None,
            extension: None,
            min_size: None,
            max_size: None,
            modified_after: None,
            is_dir: None,
        }
    }
}

pub fn search<'a>(cache: &'a Cache, query: &Query) -> Vec<&'a FileEntry> {
    let candidates: Vec<&FileEntry> = match &query.name_contains {
        Some(name) => cache.search_by_name(name).collect(),
        None => cache.iter().collect(),
    };

    candidates.into_iter()
        .filter(|e| matches_extension(e, query))
        .filter(|e| matches_size(e, query))
        .filter(|e| matches_modified(e, query))
        .filter(|e| matches_is_dir(e, query))
        .collect()
}

fn matches_extension(entry: &FileEntry, query: &Query) -> bool {
    match (&query.extension, &entry.extension) {
        (Some(q_ext), Some(e_ext)) => e_ext.to_lowercase() == q_ext.to_lowercase(),
        (Some(_), None) => false,
        (None, _) => true,
    }
}

fn matches_size(entry: &FileEntry, query: &Query) -> bool {
    if let Some(min) = query.min_size {
        if entry.size < min { return false; }
    }
    if let Some(max) = query.max_size {
        if entry.size > max { return false; }
    }
    true
}

fn matches_modified(entry: &FileEntry, query: &Query) -> bool {
    match query.modified_after {
        Some(after) => entry.modified > after,
        None => true,
    }
}

fn matches_is_dir(entry: &FileEntry, query: &Query) -> bool {
    match query.is_dir {
        Some(is_dir) => entry.is_dir == is_dir,
        None => true,
    }
}