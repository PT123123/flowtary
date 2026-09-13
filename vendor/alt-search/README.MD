# AltSearch

A fast file search tool for Windows, built in Rust. Created out of frustration with Windows' slow built-in file search.

## Why

Windows file search is painfully slow. AltSearch indexes your files into a cache so searches are near instant after the first index.

## Features

- **Fast indexing** — parallel directory traversal with `jwalk`
- **Persistent cache** — indexes once, loads instantly on subsequent runs
- **Compressed storage** — cache compressed with `zstd`
- **File system watcher** — monitors directories for changes and keeps the cache fresh automatically
- **Flexible search** — filter by name, extension, size, date modified, and type
- **Library crate** — usable as a library for other projects (e.g. a GUI frontend)

## Usage

```
altsearch [OPTIONS]
```

### Options

| Flag | Description |
|---|---|
| `-d, --dir <DIR>` | Directory to index |
| `-n, --name <NAME>` | Search by filename (case insensitive) |
| `-e, --ext <EXT>` | Filter by file extension |
| `--min-size <BYTES>` | Filter by minimum file size in bytes |
| `--max-size <BYTES>` | Filter by maximum file size in bytes |
| `--dirs-only` | Show directories only |
| `--files-only` | Show files only |
| `--reindex` | Force a fresh index of the directory |

### Examples

**First run — index a directory and search:**
```
altsearch --dir "C:\Users\Richard\Documents" --name "report"
```

**Subsequent runs — load from cache instantly:**
```
altsearch --name "report"
```

**Filter by extension:**
```
altsearch --name "config" --ext "json"
```

**Search by size range:**
```
altsearch --ext "mp4" --min-size 1000000
```

**Index entire C drive:**
```
altsearch --dir "C:\" --reindex
```

## Cache

The cache is stored at:
```
C:\Users\<name>\AppData\Roaming\AltSearch\cache.bin
```

It is compressed with `zstd` and persists between runs. Use `--reindex` to force a rebuild.

## Performance

Tested on a full C drive (~1.6 million entries):

| Operation | Time |
|---|---|
| First index | ~77s |
| Cache load | ~6s |
| Search | ~400ms |

## Tech Stack

| Crate | Purpose |
|---|---|
| `jwalk` | Parallel directory traversal |
| `rayon` | Data parallelism |
| `serde` + `postcard` | Cache serialization |
| `zstd` | Cache compression |
| `ahash` | Fast HashMap hashing |
| `notify` | File system watching |
| `clap` | CLI argument parsing |

## Project Structure

```
src/
├── main.rs       — CLI entry point
├── lib.rs        — Library entry point
├── cache.rs      — File indexing and cache management
├── search.rs     — Query and search logic
├── watcher.rs    — File system watcher
└── cli.rs        — Argument parsing and command handling
```