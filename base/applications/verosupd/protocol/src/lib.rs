/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/protocol/src/lib.rs
 * PURPOSE:     Update check-in, bootstrap, and package contract
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

//! Wire contract shared by the update client. The closed-source server speaks
//! the same JSON and container layouts without linking this crate.

mod chunk;
mod codec;
mod package;
mod schedule;

pub use chunk::{chunk_bytes, Chunk};
pub use codec::{apply_delta, diff_delta, Codec};
pub use package::{read_vpkg, write_vpkg, FilePayload, PackageFile};
pub use schedule::{jitter_offset, missed_window, PollMode, Schedule};

use serde::{Deserialize, Serialize};

pub const DEFAULT_BOOTSTRAP_URL: &str = "https://bootstrap.invalid";
pub const DEFAULT_UPDATE_URL: &str = "https://update.invalid";
pub const BOOTSTRAP_PATH: &str = "/v1/bootstrap";
pub const CHECKIN_PATH: &str = "/v1/checkin";
pub const RETENTION_DEFAULT_DAYS: u32 = 90;
pub const RETENTION_MIN_DAYS: u32 = 1;
pub const RETENTION_MAX_DAYS: u32 = 365;
pub const JITTER_MINUTES: i64 = 15;

pub fn client_dns_name(install_id: &str, suffix: &str) -> String {
    format!("{install_id}.{suffix}")
}

pub fn join_origin(origin: &str, path: &str) -> String {
    format!("{}{}", origin.trim_end_matches('/'), path)
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct BootstrapRequest {
    pub install_id: String,
    pub hardware_hash: String,
    pub license_key: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub csr_pem: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "status", rename_all = "snake_case")]
pub enum BootstrapResponse {
    Name { dns_name: String },
    Issued {
        dns_name: String,
        certificate_pem: String,
        chain_pem: String,
    },
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct InstalledPackage {
    pub name: String,
    pub version: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CheckinRequest {
    pub install_id: String,
    pub hardware_hash: String,
    pub license_key: String,
    pub track: String,
    pub packages: Vec<InstalledPackage>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RemoteFile {
    pub path: String,
    pub sha256: String,
    pub url: String,
    pub size: u64,
    pub codec: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RemotePackage {
    pub name: String,
    pub version: String,
    pub depends: Vec<String>,
    pub files: Vec<RemoteFile>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CheckinResponse {
    pub packages: Vec<RemotePackage>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ErrorBody {
    pub error: String,
}

pub fn sha256_hex(bytes: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    hex::encode(Sha256::digest(bytes))
}

pub fn https_url(url: &str) -> bool {
    url.starts_with("https://")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn joins_origins_and_names() {
        assert_eq!(
            join_origin("https://updates.example/", CHECKIN_PATH),
            "https://updates.example/v1/checkin"
        );
        assert_eq!(
            client_dns_name("abc", "clients.example"),
            "abc.clients.example"
        );
    }

    #[test]
    fn bootstrap_round_trip() {
        let request = BootstrapRequest {
            install_id: "abc".into(),
            hardware_hash: "hw".into(),
            license_key: "key".into(),
            csr_pem: None,
        };
        let json = serde_json::to_string(&request).unwrap();
        let back: BootstrapRequest = serde_json::from_str(&json).unwrap();
        assert_eq!(back, request);
        let issued = BootstrapResponse::Issued {
            dns_name: "abc.clients.example".into(),
            certificate_pem: "cert".into(),
            chain_pem: "chain".into(),
        };
        let back: BootstrapResponse = serde_json::from_str(&serde_json::to_string(&issued).unwrap()).unwrap();
        assert_eq!(back, issued);
    }
}
