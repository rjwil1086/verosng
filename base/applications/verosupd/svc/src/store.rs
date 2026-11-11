/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/svc/src/store.rs
 * PURPOSE:     Package database and content-addressed rollback chunks
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

use rusqlite::{params, Connection, OptionalExtension};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};
use verosupd_protocol::{chunk_bytes, sha256_hex};

pub struct PackageDb {
    conn: Connection,
    root: PathBuf,
}

#[derive(Debug, Clone)]
pub struct Installed {
    pub name: String,
    pub version: String,
}

impl PackageDb {
    pub fn open(root: &Path) -> Result<Self, String> {
        fs::create_dir_all(root.join("chunks")).map_err(|err| err.to_string())?;
        let conn = Connection::open(root.join("pkgdb.db")).map_err(|err| err.to_string())?;
        conn.execute_batch(
            "CREATE TABLE IF NOT EXISTS packages (
                name TEXT PRIMARY KEY,
                version TEXT NOT NULL,
                depends TEXT NOT NULL DEFAULT ''
            );
            CREATE TABLE IF NOT EXISTS files (
                path TEXT PRIMARY KEY,
                package TEXT NOT NULL,
                sha256 TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS rollback_files (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                package TEXT NOT NULL,
                from_version TEXT NOT NULL,
                path TEXT NOT NULL,
                created_at INTEGER NOT NULL,
                empty INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS rollback_chunks (
                rollback_id INTEGER NOT NULL,
                seq INTEGER NOT NULL,
                hash TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS chunks (
                hash TEXT PRIMARY KEY,
                refcount INTEGER NOT NULL
            );",
        )
        .map_err(|err| err.to_string())?;
        Ok(Self {
            conn,
            root: root.to_path_buf(),
        })
    }

    pub fn list(&self) -> Result<Vec<Installed>, String> {
        let mut stmt = self
            .conn
            .prepare("SELECT name, version FROM packages ORDER BY name")
            .map_err(|err| err.to_string())?;
        let rows = stmt
            .query_map([], |row| {
                Ok(Installed {
                    name: row.get(0)?,
                    version: row.get(1)?,
                })
            })
            .map_err(|err| err.to_string())?;
        rows.collect::<Result<Vec<_>, _>>().map_err(|err| err.to_string())
    }

    pub fn remember_previous(&self, package: &str, from_version: &str, path: &str, previous: Option<&[u8]>) -> Result<(), String> {
        let created = unix_now();
        let empty = previous.is_none();
        self.conn
            .execute(
                "INSERT INTO rollback_files (package, from_version, path, created_at, empty) VALUES (?1, ?2, ?3, ?4, ?5)",
                params![package, from_version, path, created, empty as i32],
            )
            .map_err(|err| err.to_string())?;
        let id = self.conn.last_insert_rowid();
        let Some(bytes) = previous else {
            return Ok(());
        };
        let mut seq = 0;
        for chunk in chunk_bytes(bytes) {
            let slice = &bytes[chunk.offset..chunk.offset + chunk.len];
            let hash = sha256_hex(slice);
            self.store_chunk(&hash, slice)?;
            self.conn
                .execute(
                    "INSERT INTO rollback_chunks (rollback_id, seq, hash) VALUES (?1, ?2, ?3)",
                    params![id, seq, hash],
                )
                .map_err(|err| err.to_string())?;
            seq += 1;
        }
        Ok(())
    }

    pub fn set_file(&self, package: &str, path: &str, sha256: &str) -> Result<(), String> {
        self.conn
            .execute(
                "INSERT INTO files (path, package, sha256) VALUES (?1, ?2, ?3)
                 ON CONFLICT(path) DO UPDATE SET package = excluded.package, sha256 = excluded.sha256",
                params![path, package, sha256],
            )
            .map_err(|err| err.to_string())?;
        Ok(())
    }

    pub fn set_package(&self, name: &str, version: &str, depends: &[String]) -> Result<(), String> {
        self.conn
            .execute(
                "INSERT INTO packages (name, version, depends) VALUES (?1, ?2, ?3)
                 ON CONFLICT(name) DO UPDATE SET version = excluded.version, depends = excluded.depends",
                params![name, version, depends.join(",")],
            )
            .map_err(|err| err.to_string())?;
        Ok(())
    }

    pub fn rollback(&self, package: &str) -> Result<Vec<(String, Option<Vec<u8>>)>, String> {
        let mut stmt = self
            .conn
            .prepare(
                "SELECT id, path, empty FROM rollback_files
                 WHERE package = ?1
                 AND created_at = (SELECT MAX(created_at) FROM rollback_files WHERE package = ?1)",
            )
            .map_err(|err| err.to_string())?;
        let rows = stmt
            .query_map(params![package], |row| Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?, row.get::<_, i32>(2)?)))
            .map_err(|err| err.to_string())?
            .collect::<Result<Vec<_>, _>>()
            .map_err(|err| err.to_string())?;
        if rows.is_empty() {
            return Err(format!("no rollback for {package}"));
        }
        let mut restored = Vec::new();
        for (id, path, empty) in rows {
            if empty != 0 {
                restored.push((path, None));
                continue;
            }
            let mut chunks = self
                .conn
                .prepare("SELECT hash FROM rollback_chunks WHERE rollback_id = ?1 ORDER BY seq")
                .map_err(|err| err.to_string())?;
            let hashes = chunks
                .query_map(params![id], |row| row.get::<_, String>(0))
                .map_err(|err| err.to_string())?
                .collect::<Result<Vec<_>, _>>()
                .map_err(|err| err.to_string())?;
            let mut bytes = Vec::new();
            for hash in hashes {
                bytes.extend_from_slice(&self.read_chunk(&hash)?);
            }
            restored.push((path, Some(bytes)));
        }
        Ok(restored)
    }

    pub fn version_before(&self, package: &str) -> Result<Option<String>, String> {
        self.conn
            .query_row(
                "SELECT from_version FROM rollback_files WHERE package = ?1 ORDER BY created_at DESC LIMIT 1",
                params![package],
                |row| row.get(0),
            )
            .optional()
            .map_err(|err| err.to_string())
    }

    pub fn prune(&self, retention_days: u32) -> Result<usize, String> {
        let cutoff = unix_now() - retention_days as i64 * 86_400;
        let mut stmt = self
            .conn
            .prepare("SELECT id FROM rollback_files WHERE created_at < ?1")
            .map_err(|err| err.to_string())?;
        let ids = stmt
            .query_map(params![cutoff], |row| row.get::<_, i64>(0))
            .map_err(|err| err.to_string())?
            .collect::<Result<Vec<_>, _>>()
            .map_err(|err| err.to_string())?;
        for id in &ids {
            self.release_rollback(*id)?;
        }
        Ok(ids.len())
    }

    pub fn reset(&self) -> Result<(), String> {
        self.conn
            .execute_batch(
                "DELETE FROM rollback_chunks;
                 DELETE FROM rollback_files;
                 DELETE FROM chunks;
                 DELETE FROM files;
                 DELETE FROM packages;",
            )
            .map_err(|err| err.to_string())?;
        let chunks = self.root.join("chunks");
        if chunks.exists() {
            fs::remove_dir_all(&chunks).map_err(|err| err.to_string())?;
        }
        fs::create_dir_all(chunks).map_err(|err| err.to_string())?;
        Ok(())
    }

    fn store_chunk(&self, hash: &str, bytes: &[u8]) -> Result<(), String> {
        let existing: Option<i64> = self
            .conn
            .query_row("SELECT refcount FROM chunks WHERE hash = ?1", params![hash], |row| row.get(0))
            .optional()
            .map_err(|err| err.to_string())?;
        if let Some(refcount) = existing {
            self.conn
                .execute(
                    "UPDATE chunks SET refcount = ?1 WHERE hash = ?2",
                    params![refcount + 1, hash],
                )
                .map_err(|err| err.to_string())?;
            return Ok(());
        }
        let compressed = zstd::encode_all(bytes, 3).map_err(|err| err.to_string())?;
        fs::write(self.chunk_path(hash), compressed).map_err(|err| err.to_string())?;
        self.conn
            .execute("INSERT INTO chunks (hash, refcount) VALUES (?1, 1)", params![hash])
            .map_err(|err| err.to_string())?;
        Ok(())
    }

    fn read_chunk(&self, hash: &str) -> Result<Vec<u8>, String> {
        let compressed = fs::read(self.chunk_path(hash)).map_err(|err| err.to_string())?;
        zstd::decode_all(compressed.as_slice()).map_err(|err| err.to_string())
    }

    fn release_rollback(&self, id: i64) -> Result<(), String> {
        let mut stmt = self
            .conn
            .prepare("SELECT hash FROM rollback_chunks WHERE rollback_id = ?1")
            .map_err(|err| err.to_string())?;
        let hashes = stmt
            .query_map(params![id], |row| row.get::<_, String>(0))
            .map_err(|err| err.to_string())?
            .collect::<Result<Vec<_>, _>>()
            .map_err(|err| err.to_string())?;
        for hash in hashes {
            let count: i64 = self
                .conn
                .query_row("SELECT refcount FROM chunks WHERE hash = ?1", params![hash], |row| row.get(0))
                .unwrap_or(0);
            if count <= 1 {
                let _ = fs::remove_file(self.chunk_path(&hash));
                self.conn
                    .execute("DELETE FROM chunks WHERE hash = ?1", params![hash])
                    .map_err(|err| err.to_string())?;
            } else {
                self.conn
                    .execute(
                        "UPDATE chunks SET refcount = ?1 WHERE hash = ?2",
                        params![count - 1, hash],
                    )
                    .map_err(|err| err.to_string())?;
            }
        }
        self.conn
            .execute("DELETE FROM rollback_chunks WHERE rollback_id = ?1", params![id])
            .map_err(|err| err.to_string())?;
        self.conn
            .execute("DELETE FROM rollback_files WHERE id = ?1", params![id])
            .map_err(|err| err.to_string())?;
        Ok(())
    }

    fn chunk_path(&self, hash: &str) -> PathBuf {
        self.root.join("chunks").join(format!("{hash}.zst"))
    }
}

fn unix_now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn duplicate_reverse_chunks_share_one_file() {
        let root = std::env::temp_dir().join(format!("verosupd-db-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let db = PackageDb::open(&root).unwrap();
        let body = vec![9u8; 20_000];
        db.remember_previous("veros-shell", "1", r"system32\a.dll", Some(&body))
            .unwrap();
        let first = fs::read_dir(root.join("chunks")).unwrap().count();
        db.remember_previous("veros-kernel", "1", r"system32\b.dll", Some(&body))
            .unwrap();
        let second = fs::read_dir(root.join("chunks")).unwrap().count();
        assert_eq!(first, second);
        assert!(first > 0);
        let restored = db.rollback("veros-shell").unwrap();
        assert_eq!(restored[0].1.as_deref(), Some(body.as_slice()));
        let _ = fs::remove_dir_all(&root);
    }
}
