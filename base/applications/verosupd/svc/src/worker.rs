/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/svc/src/worker.rs
 * PURPOSE:     Check-in, package apply, rollback, and the command line
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

use crate::config::{clamp_retention, load_settings, save_settings, Settings};
use crate::enroll::ensure_certificate;
use crate::store::PackageDb;
use std::fs;
use std::path::Path;
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use verosupd_protocol::{
    apply_delta, https_url, join_origin, missed_window, read_vpkg, sha256_hex, write_vpkg, CheckinRequest,
    CheckinResponse, FilePayload, InstalledPackage, PollMode, CHECKIN_PATH,
};

#[derive(Debug)]
pub enum WorkerError {
    Message(String),
}

impl std::fmt::Display for WorkerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            WorkerError::Message(text) => f.write_str(text),
        }
    }
}

pub fn serve_until(stop: impl Fn() -> bool) {
    let mut caught_up = false;
    while !stop() {
        let settings = load_settings();
        if !caught_up {
            caught_up = true;
            let now = unix_now();
            if missed_window(now, settings.last_success_unix, &settings.schedule) {
                thread::sleep(Duration::from_secs(5));
                let _ = check_in(&load_settings(), false);
            }
        }
        let settings = load_settings();
        if settings.schedule.mode == PollMode::Automatic {
            let now = unix_now();
            if missed_window(now, settings.last_success_unix, &settings.schedule) {
                let _ = check_in(&settings, false);
            }
        }
        thread::sleep(Duration::from_secs(30));
    }
}

pub fn check_in(settings: &Settings, force_download: bool) -> Result<Vec<String>, WorkerError> {
    if settings.schedule.mode == PollMode::Disabled && !force_download {
        return Ok(Vec::new());
    }
    if settings.license_key.is_empty() {
        return Err(WorkerError::Message("no license key".into()));
    }
    ensure_certificate(settings).map_err(WorkerError::Message)?;
    let db = PackageDb::open(&settings.state_dir).map_err(WorkerError::Message)?;
    let installed = db.list().map_err(WorkerError::Message)?;
    let (_, update) = settings.origins();
    let url = join_origin(&update, CHECKIN_PATH);
    let request = CheckinRequest {
        install_id: settings.install_id.clone(),
        hardware_hash: settings.hardware_hash.clone(),
        license_key: settings.license_key.clone(),
        track: settings.track.clone(),
        packages: installed
            .iter()
            .map(|item| InstalledPackage {
                name: item.name.clone(),
                version: item.version.clone(),
            })
            .collect(),
    };
    let cert = fs::read(settings.state_dir.join("client.crt")).map_err(|err| WorkerError::Message(err.to_string()))?;
    let key = fs::read(settings.state_dir.join("client.key")).map_err(|err| WorkerError::Message(err.to_string()))?;
    let identity = reqwest::Identity::from_pem(&[cert, key].concat()).map_err(|err| WorkerError::Message(err.to_string()))?;
    let client = reqwest::blocking::Client::builder()
        .identity(identity)
        .danger_accept_invalid_certs(false)
        .timeout(Duration::from_secs(60))
        .build()
        .map_err(|err| WorkerError::Message(err.to_string()))?;
    let response = client
        .post(&url)
        .json(&request)
        .send()
        .map_err(|err| WorkerError::Message(err.to_string()))?;
    if response.status().as_u16() == 402 {
        return Err(WorkerError::Message("license inactive".into()));
    }
    let body: CheckinResponse = response
        .error_for_status()
        .map_err(|err| WorkerError::Message(err.to_string()))?
        .json()
        .map_err(|err| WorkerError::Message(err.to_string()))?;
    let mut applied = Vec::new();
    let defer_downloads = settings.metered && !force_download;
    if !defer_downloads {
        for package in body.packages {
            apply_package(settings, &db, &client, &package.name, &package.version, &package.depends, &package.files)
                .map_err(WorkerError::Message)?;
            applied.push(package.name);
        }
    }
    let mut saved = settings.clone();
    saved.last_success_unix = Some(unix_now());
    save_settings(&saved).map_err(WorkerError::Message)?;
    let _ = db.prune(settings.retention_days);
    Ok(applied)
}

pub fn apply_downloaded(settings: &Settings, bytes: &[u8]) -> Result<(), String> {
    let db = PackageDb::open(&settings.state_dir)?;
    let (name, version, depends, files) = read_vpkg(bytes).map_err(|_| "package is not a vpkg".to_string())?;
    apply_payloads(settings, &db, &name, &version, &depends, &files)
}

fn apply_package(
    settings: &Settings,
    db: &PackageDb,
    client: &reqwest::blocking::Client,
    name: &str,
    version: &str,
    depends: &[String],
    files: &[verosupd_protocol::RemoteFile],
) -> Result<(), String> {
    let mut payloads = Vec::new();
    for file in files {
        if !https_url(&file.url) {
            return Err(format!("refusing non-https payload {}", file.url));
        }
        let bytes = client.get(&file.url).send().map_err(|err| err.to_string())?.bytes().map_err(|err| err.to_string())?;
        let plain = if file.codec == "vdelta" {
            let current = fs::read(system_path(&file.path)).unwrap_or_default();
            apply_delta(&current, &bytes).map_err(|_| format!("delta failed for {}", file.path))?
        } else if file.codec == "vpkg-file" {
            zstd::decode_all(bytes.as_ref()).map_err(|err| err.to_string())?
        } else {
            return Err(format!("unknown codec {}", file.codec));
        };
        if sha256_hex(&plain) != file.sha256 {
            return Err(format!("hash mismatch for {}", file.path));
        }
        payloads.push(FilePayload {
            path: file.path.clone(),
            bytes: plain,
        });
    }
    apply_payloads(settings, db, name, version, depends, &payloads)
}

fn apply_payloads(settings: &Settings, db: &PackageDb, name: &str, version: &str, depends: &[String], files: &[FilePayload]) -> Result<(), String> {
    let previous_version = db
        .list()?
        .into_iter()
        .find(|item| item.name == name)
        .map(|item| item.version)
        .unwrap_or_else(|| "0".into());
    for file in files {
        let path = system_path(&file.path);
        let previous = fs::read(&path).ok();
        db.remember_previous(name, &previous_version, &file.path, previous.as_deref())?;
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(|err| err.to_string())?;
        }
        fs::write(&path, &file.bytes).map_err(|err| err.to_string())?;
        db.set_file(name, &file.path, &sha256_hex(&file.bytes))?;
    }
    db.set_package(name, version, depends)?;
    let _ = settings;
    Ok(())
}

pub fn rollback_package(settings: &Settings, package: &str) -> Result<(), String> {
    let db = PackageDb::open(&settings.state_dir)?;
    let restored = db.rollback(package)?;
    for (path, bytes) in restored {
        let full = system_path(&path);
        match bytes {
            Some(bytes) => {
                fs::write(&full, &bytes).map_err(|err| err.to_string())?;
                db.set_file(package, &path, &sha256_hex(&bytes))?;
            }
            None => {
                let _ = fs::remove_file(&full);
            }
        }
    }
    if let Some(version) = db.version_before(package)? {
        db.set_package(package, &version, &[])?;
    }
    Ok(())
}

pub fn reset_database(settings: &Settings) -> Result<usize, String> {
    let db = PackageDb::open(&settings.state_dir)?;
    db.reset()?;
    let root = std::env::var("VEROSUPD_SCAN").unwrap_or_else(|_| r"C:\VerOS\System32".into());
    let mut count = 0;
    scan_baseline(&db, Path::new(&root), Path::new(&root), &mut count)?;
    Ok(count)
}

fn scan_baseline(db: &PackageDb, root: &Path, dir: &Path, count: &mut usize) -> Result<(), String> {
    let entries = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(_) => return Ok(()),
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            scan_baseline(db, root, &path, count)?;
            continue;
        }
        let bytes = fs::read(&path).map_err(|err| err.to_string())?;
        let relative = path.strip_prefix(root).unwrap_or(&path);
        let text = relative.to_string_lossy().replace('/', "\\");
        db.set_file("baseline", &text, &sha256_hex(&bytes))?;
        *count += 1;
    }
    db.set_package("baseline", "0", &[])?;
    Ok(())
}

pub fn system_path(relative: &str) -> std::path::PathBuf {
    let root = std::env::var("VEROSUPD_ROOT").unwrap_or_else(|_| r"C:\VerOS".into());
    Path::new(&root).join(relative.replace('/', "\\"))
}

pub fn cli(args: &[String]) -> i32 {
    let command = args.get(1).map(String::as_str).unwrap_or("/help");
    let result = match command {
        "/check" | "--check" => check_in(&load_settings(), false).map(|items| format!("checked {}", items.len())),
        "/apply" | "--apply" => args.get(2).map(|path| apply_path(&load_settings(), path)).unwrap_or_else(|| {
            check_in(&load_settings(), true).map(|items| format!("applied {}", items.len()))
        }),
        "/list" | "--list" => list_packages(),
        "/rollback" | "--rollback" => args
            .get(2)
            .ok_or_else(|| WorkerError::Message("usage: /rollback <package>".into()))
            .and_then(|package| rollback_package(&load_settings(), package).map_err(WorkerError::Message).map(|()| format!("rolled back {package}"))),
        "/resetdb" | "--resetdb" => reset_database(&load_settings())
            .map(|count| format!("baseline files {count}"))
            .map_err(WorkerError::Message),
        "/license" | "--license" => set_license(args.get(3).map(String::as_str)),
        "/config" | "--config" => configure(&args[1..]),
        _ => Ok(help()),
    };
    match result {
        Ok(text) => {
            println!("{text}");
            0
        }
        Err(err) => {
            eprintln!("{err}");
            1
        }
    }
}

fn apply_path(settings: &Settings, path: &str) -> Result<String, WorkerError> {
    let bytes = fs::read(path).map_err(|err| WorkerError::Message(err.to_string()))?;
    apply_downloaded(settings, &bytes).map_err(WorkerError::Message)?;
    Ok(format!("applied {path}"))
}

fn list_packages() -> Result<String, WorkerError> {
    let settings = load_settings();
    let db = PackageDb::open(&settings.state_dir).map_err(WorkerError::Message)?;
    let lines: Vec<String> = db
        .list()
        .map_err(WorkerError::Message)?
        .into_iter()
        .map(|item| format!("{} {}", item.name, item.version))
        .collect();
    Ok(if lines.is_empty() { "no packages".into() } else { lines.join("\n") })
}

fn set_license(key: Option<&str>) -> Result<String, WorkerError> {
    let Some(key) = key else {
        return Err(WorkerError::Message("usage: /license set <key>".into()));
    };
    let mut settings = load_settings();
    settings.license_key = key.to_string();
    save_settings(&settings).map_err(WorkerError::Message)?;
    Ok("license saved".into())
}

fn configure(args: &[String]) -> Result<String, WorkerError> {
    let mut settings = load_settings();
    match args.get(1).map(String::as_str) {
        Some("/config") | Some("--config") | None => {}
        _ => {}
    }
    let action = args.get(2).map(String::as_str).unwrap_or("show");
    match action {
        "show" => {
            let (boot, upd) = settings.origins();
            return Ok(format!(
                "bootstrap {boot}\nupdate {upd}\nretention {}\nmode {:?}",
                settings.retention_days, settings.schedule.mode
            ));
        }
        "bootstrap" => {
            settings.bootstrap_url = args.get(3).cloned().unwrap_or_default();
        }
        "update" => {
            settings.update_url = args.get(3).cloned().unwrap_or_default();
        }
        "clear-urls" => {
            settings.bootstrap_url.clear();
            settings.update_url.clear();
        }
        "retention" => {
            let days = args
                .get(3)
                .and_then(|text| text.parse().ok())
                .ok_or_else(|| WorkerError::Message("usage: /config retention <days>".into()))?;
            settings.retention_days = clamp_retention(days);
        }
        "mode" => {
            settings.schedule.mode = match args.get(3).map(String::as_str).unwrap_or("") {
                "disabled" => PollMode::Disabled,
                "manual" => PollMode::Manual,
                _ => PollMode::Automatic,
            };
        }
        _ => return Err(WorkerError::Message("usage: /config show|bootstrap|update|clear-urls|retention|mode".into())),
    }
    save_settings(&settings).map_err(WorkerError::Message)?;
    let (boot, upd) = settings.origins();
    Ok(format!("bootstrap {boot}\nupdate {upd}"))
}

fn help() -> String {
    "/check /apply /list /rollback <pkg> /resetdb /config /license set <key>".into()
}

pub fn pack_for_test(name: &str, version: &str, path: &str, bytes: &[u8]) -> Vec<u8> {
    write_vpkg(
        name,
        version,
        &[],
        &[FilePayload {
            path: path.into(),
            bytes: bytes.to_vec(),
        }],
    )
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
    fn apply_rollback_and_reset() {
        let root = std::env::temp_dir().join(format!("verosupd-work-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("system32")).unwrap();
        std::env::set_var("VEROSUPD_STATE", &root);
        std::env::set_var("VEROSUPD_ROOT", &root);
        std::env::set_var("VEROSUPD_SCAN", root.join("system32"));
        let mut settings = load_settings();
        settings.state_dir = root.clone();
        settings.install_id = "test-install".into();
        save_settings(&settings).unwrap();
        let blob = pack_for_test("veros-shell", "2", "system32\\shell.dll", b"new-bytes");
        apply_downloaded(&settings, &blob).unwrap();
        let written = fs::read(root.join("system32\\shell.dll")).unwrap();
        assert_eq!(written, b"new-bytes");
        rollback_package(&settings, "veros-shell").unwrap();
        assert!(!root.join("system32\\shell.dll").exists());
        fs::write(root.join("system32\\keep.bin"), b"kept").unwrap();
        let count = reset_database(&settings).unwrap();
        assert!(count >= 1);
        let _ = fs::remove_dir_all(&root);
    }
}
