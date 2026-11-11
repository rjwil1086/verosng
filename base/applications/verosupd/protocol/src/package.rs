/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/protocol/src/package.rs
 * PURPOSE:     Whole-file .vpkg container
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

use crate::sha256_hex;
use serde::{Deserialize, Serialize};

pub const VPKG_MAGIC: &[u8; 4] = b"VPKG";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PackageFile {
    pub path: String,
    pub sha256: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FilePayload {
    pub path: String,
    pub bytes: Vec<u8>,
}

#[derive(Debug, Serialize, Deserialize)]
struct Manifest {
    name: String,
    version: String,
    depends: Vec<String>,
    files: Vec<PackageFile>,
}

#[derive(Debug)]
pub enum PackageError {
    BadMagic,
    Truncated,
    HashMismatch,
}

pub fn write_vpkg(name: &str, version: &str, depends: &[String], files: &[FilePayload]) -> Vec<u8> {
    let listed = files
        .iter()
        .map(|file| PackageFile {
            path: file.path.clone(),
            sha256: sha256_hex(&file.bytes),
        })
        .collect();
    let manifest = Manifest {
        name: name.into(),
        version: version.into(),
        depends: depends.to_vec(),
        files: listed,
    };
    let json = serde_json::to_vec(&manifest).expect("manifest");
    let mut out = Vec::new();
    out.extend_from_slice(VPKG_MAGIC);
    out.extend_from_slice(&1u32.to_le_bytes());
    out.extend_from_slice(&(json.len() as u32).to_le_bytes());
    out.extend_from_slice(&json);
    for file in files {
        let compressed = zstd::encode_all(file.bytes.as_slice(), 3).expect("zstd");
        out.extend_from_slice(&(compressed.len() as u64).to_le_bytes());
        out.extend_from_slice(&compressed);
    }
    out
}

pub fn read_vpkg(bytes: &[u8]) -> Result<(String, String, Vec<String>, Vec<FilePayload>), PackageError> {
    if bytes.len() < 12 || &bytes[0..4] != VPKG_MAGIC {
        return Err(PackageError::BadMagic);
    }
    let version = u32::from_le_bytes(bytes[4..8].try_into().unwrap());
    if version != 1 {
        return Err(PackageError::BadMagic);
    }
    let json_len = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
    if 12 + json_len > bytes.len() {
        return Err(PackageError::Truncated);
    }
    let manifest: Manifest = serde_json::from_slice(&bytes[12..12 + json_len]).map_err(|_| PackageError::Truncated)?;
    let mut cursor = 12 + json_len;
    let mut files = Vec::new();
    for listed in &manifest.files {
        if cursor + 8 > bytes.len() {
            return Err(PackageError::Truncated);
        }
        let len = u64::from_le_bytes(bytes[cursor..cursor + 8].try_into().unwrap()) as usize;
        cursor += 8;
        if cursor + len > bytes.len() {
            return Err(PackageError::Truncated);
        }
        let plain = zstd::decode_all(&bytes[cursor..cursor + len]).map_err(|_| PackageError::Truncated)?;
        cursor += len;
        if sha256_hex(&plain) != listed.sha256 {
            return Err(PackageError::HashMismatch);
        }
        files.push(FilePayload {
            path: listed.path.clone(),
            bytes: plain,
        });
    }
    Ok((manifest.name, manifest.version, manifest.depends, files))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn package_round_trip() {
        let files = vec![FilePayload {
            path: r"system32\sample.dll".into(),
            bytes: b"hello veros".to_vec(),
        }];
        let blob = write_vpkg("veros-shell", "2", &[], &files);
        let (name, version, _, back) = read_vpkg(&blob).unwrap();
        assert_eq!(name, "veros-shell");
        assert_eq!(version, "2");
        assert_eq!(back, files);
    }
}
