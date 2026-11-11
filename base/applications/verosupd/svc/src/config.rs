/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/svc/src/config.rs
 * PURPOSE:     Update service settings in the registry, with a file mirror
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use verosupd_protocol::{
    PollMode, Schedule, DEFAULT_BOOTSTRAP_URL, DEFAULT_UPDATE_URL, RETENTION_DEFAULT_DAYS,
    RETENTION_MAX_DAYS, RETENTION_MIN_DAYS,
};
use windows_sys::Win32::System::Registry::{
    RegCloseKey, RegCreateKeyExW, RegOpenKeyExW, RegQueryValueExW, RegSetValueExW, HKEY,
    HKEY_LOCAL_MACHINE, KEY_READ, KEY_WRITE, REG_DWORD, REG_SZ, REG_OPTION_NON_VOLATILE,
};

const KEY_PATH: &str = "SOFTWARE\\VerOS\\UpdateService";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct Settings {
    pub schedule: Schedule,
    pub track: String,
    pub last_success_unix: Option<i64>,
    pub license_key: String,
    pub bootstrap_url: String,
    pub update_url: String,
    pub retention_days: u32,
    pub install_id: String,
    pub hardware_hash: String,
    pub metered: bool,
    pub state_dir: PathBuf,
}

impl Settings {
    pub fn origins(&self) -> (String, String) {
        let bootstrap = if self.bootstrap_url.is_empty() {
            DEFAULT_BOOTSTRAP_URL
        } else {
            &self.bootstrap_url
        };
        let update = if self.update_url.is_empty() {
            DEFAULT_UPDATE_URL
        } else {
            &self.update_url
        };
        (bootstrap.to_string(), update.to_string())
    }
}

pub fn load_settings() -> Settings {
    if let Ok(dir) = std::env::var("VEROSUPD_STATE") {
        let path = PathBuf::from(dir);
        return load_file(&path).unwrap_or_else(|| defaults(&path));
    }
    let mut settings = read_registry().unwrap_or_else(|| defaults(&default_state_dir()));
    if settings.install_id.is_empty() {
        settings.install_id = uuid_like();
        let _ = save_settings(&settings);
    }
    if settings.hardware_hash.is_empty() {
        settings.hardware_hash = hardware_hash();
        let _ = save_settings(&settings);
    }
    settings
}

pub fn save_settings(settings: &Settings) -> Result<(), String> {
    fs::create_dir_all(&settings.state_dir).map_err(|err| err.to_string())?;
    let json = serde_json::to_vec_pretty(settings).map_err(|err| err.to_string())?;
    fs::write(settings.state_dir.join("settings.json"), json).map_err(|err| err.to_string())?;
    match write_registry(settings) {
        Ok(()) => Ok(()),
        Err(_) => Ok(()),
    }
}

pub fn clamp_retention(days: u32) -> u32 {
    days.clamp(RETENTION_MIN_DAYS, RETENTION_MAX_DAYS)
}

fn defaults(state_dir: &Path) -> Settings {
    Settings {
        schedule: Schedule::default(),
        track: "stable".into(),
        last_success_unix: None,
        license_key: String::new(),
        bootstrap_url: String::new(),
        update_url: String::new(),
        retention_days: RETENTION_DEFAULT_DAYS,
        install_id: String::new(),
        hardware_hash: String::new(),
        metered: false,
        state_dir: state_dir.to_path_buf(),
    }
}

fn default_state_dir() -> PathBuf {
    PathBuf::from(r"C:\VerOS\System32\config\verosupd")
}

fn load_file(dir: &Path) -> Option<Settings> {
    let bytes = fs::read(dir.join("settings.json")).ok()?;
    serde_json::from_slice(&bytes).ok()
}

fn uuid_like() -> String {
    use sha2::{Digest, Sha256};
    let mut hasher = Sha256::new();
    hasher.update(std::process::id().to_le_bytes());
    hasher.update(
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos().to_le_bytes())
            .unwrap_or_default(),
    );
    let digest = hasher.finalize();
    format!(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-4{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6] & 0x0f,
        digest[7], (digest[8] & 0x3f) | 0x80, digest[9], digest[10], digest[11], digest[12],
        digest[13], digest[14], digest[15]
    )
}

fn hardware_hash() -> String {
    use sha2::{Digest, Sha256};
    let mut hasher = Sha256::new();
    if let Some(guid) = reg_string(r"SOFTWARE\Microsoft\Cryptography", "MachineGuid") {
        hasher.update(guid.as_bytes());
    }
    if let Some(name) = reg_string(
        r"SYSTEM\CurrentControlSet\Control\ComputerName\ComputerName",
        "ComputerName",
    ) {
        hasher.update(name.as_bytes());
    }
    hex::encode(hasher.finalize())
}

fn read_registry() -> Option<Settings> {
    let key = open_key(KEY_READ).ok()?;
    let mut settings = defaults(&default_state_dir());
    if let Some(value) = query_sz(key, "Mode") {
        settings.schedule.mode = match value.as_str() {
            "Disabled" => PollMode::Disabled,
            "Manual" => PollMode::Manual,
            _ => PollMode::Automatic,
        };
    }
    if let Some(value) = query_dword(key, "Days") {
        settings.schedule.days = value as u8;
    }
    if let Some(value) = query_dword(key, "MinuteOfDay") {
        settings.schedule.minute_of_day = Some(value as u16);
    }
    if let Some(value) = query_dword(key, "IntervalHours") {
        settings.schedule.interval_hours = Some(value);
    }
    if let Some(value) = query_sz(key, "Track") {
        settings.track = value;
    }
    if let Some(value) = query_sz(key, "LastSuccessfulCheck") {
        settings.last_success_unix = value.parse().ok();
    }
    if let Some(value) = query_sz(key, "LicenseKey") {
        settings.license_key = value;
    }
    if let Some(value) = query_sz(key, "BootstrapUrl") {
        settings.bootstrap_url = value;
    }
    if let Some(value) = query_sz(key, "UpdateUrl") {
        settings.update_url = value;
    }
    if let Some(value) = query_dword(key, "RollbackRetentionDays") {
        settings.retention_days = clamp_retention(value);
    }
    if let Some(value) = query_sz(key, "InstallId") {
        settings.install_id = value;
    }
    if let Some(value) = query_sz(key, "HardwareHash") {
        settings.hardware_hash = value;
    }
    settings.metered = query_dword(key, "Metered").unwrap_or(0) != 0;
    if let Some(value) = query_sz(key, "StateDir") {
        settings.state_dir = PathBuf::from(value);
    }
    unsafe { RegCloseKey(key) };
    Some(settings)
}

fn write_registry(settings: &Settings) -> Result<(), String> {
    let key = open_key(KEY_READ | KEY_WRITE)?;
    let mode = match settings.schedule.mode {
        PollMode::Disabled => "Disabled",
        PollMode::Manual => "Manual",
        PollMode::Automatic => "Automatic",
    };
    set_sz(key, "Mode", mode)?;
    set_dword(key, "Days", settings.schedule.days as u32)?;
    if let Some(minute) = settings.schedule.minute_of_day {
        set_dword(key, "MinuteOfDay", minute as u32)?;
    }
    if let Some(hours) = settings.schedule.interval_hours {
        set_dword(key, "IntervalHours", hours)?;
    }
    set_sz(key, "Track", &settings.track)?;
    if let Some(last) = settings.last_success_unix {
        set_sz(key, "LastSuccessfulCheck", &last.to_string())?;
    }
    set_sz(key, "LicenseKey", &settings.license_key)?;
    set_sz(key, "BootstrapUrl", &settings.bootstrap_url)?;
    set_sz(key, "UpdateUrl", &settings.update_url)?;
    set_dword(key, "RollbackRetentionDays", settings.retention_days)?;
    set_sz(key, "InstallId", &settings.install_id)?;
    set_sz(key, "HardwareHash", &settings.hardware_hash)?;
    set_dword(key, "Metered", u32::from(settings.metered))?;
    set_sz(key, "StateDir", &settings.state_dir.display().to_string())?;
    unsafe { RegCloseKey(key) };
    Ok(())
}

fn open_key(access: u32) -> Result<HKEY, String> {
    let mut key = std::ptr::null_mut();
    let path: Vec<u16> = KEY_PATH.encode_utf16().chain(std::iter::once(0)).collect();
    let status = unsafe {
        RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            path.as_ptr(),
            0,
            std::ptr::null(),
            REG_OPTION_NON_VOLATILE,
            access,
            std::ptr::null(),
            &mut key,
            std::ptr::null_mut(),
        )
    };
    if status == 0 {
        Ok(key)
    } else {
        Err(format!("registry open failed: {status}"))
    }
}

fn query_sz(key: HKEY, name: &str) -> Option<String> {
    let name: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
    let mut kind = 0u32;
    let mut len = 0u32;
    let status = unsafe {
        RegQueryValueExW(
            key,
            name.as_ptr(),
            std::ptr::null(),
            &mut kind,
            std::ptr::null_mut(),
            &mut len,
        )
    };
    if status != 0 || kind != REG_SZ || len < 2 {
        return None;
    }
    let mut buf = vec![0u16; (len as usize) / 2];
    let status = unsafe {
        RegQueryValueExW(
            key,
            name.as_ptr(),
            std::ptr::null(),
            &mut kind,
            buf.as_mut_ptr() as *mut u8,
            &mut len,
        )
    };
    if status != 0 {
        return None;
    }
    let end = buf.iter().position(|ch| *ch == 0).unwrap_or(buf.len());
    Some(String::from_utf16_lossy(&buf[..end]))
}

fn query_dword(key: HKEY, name: &str) -> Option<u32> {
    let name: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
    let mut kind = 0u32;
    let mut len = 4u32;
    let mut value = 0u32;
    let status = unsafe {
        RegQueryValueExW(
            key,
            name.as_ptr(),
            std::ptr::null(),
            &mut kind,
            &mut value as *mut u32 as *mut u8,
            &mut len,
        )
    };
    if status == 0 && kind == REG_DWORD {
        Some(value)
    } else {
        None
    }
}

fn set_sz(key: HKEY, name: &str, value: &str) -> Result<(), String> {
    let wide: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
    let mut data: Vec<u16> = value.encode_utf16().chain(std::iter::once(0)).collect();
    let status = unsafe {
        RegSetValueExW(
            key,
            wide.as_ptr(),
            0,
            REG_SZ,
            data.as_mut_ptr() as *const u8,
            (data.len() * 2) as u32,
        )
    };
    if status == 0 {
        Ok(())
    } else {
        Err(format!("registry write {name} failed: {status}"))
    }
}

fn set_dword(key: HKEY, name: &str, value: u32) -> Result<(), String> {
    let name: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
    let status = unsafe {
        RegSetValueExW(
            key,
            name.as_ptr(),
            0,
            REG_DWORD,
            &value as *const u32 as *const u8,
            4,
        )
    };
    if status == 0 {
        Ok(())
    } else {
        Err(format!("registry write failed: {status}"))
    }
}

fn reg_string(path: &str, name: &str) -> Option<String> {
    let mut key = std::ptr::null_mut();
    let wide: Vec<u16> = path.encode_utf16().chain(std::iter::once(0)).collect();
    let status = unsafe {
        RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            wide.as_ptr(),
            0,
            KEY_READ,
            &mut key,
        )
    };
    if status != 0 {
        return None;
    }
    let value = query_sz(key, name);
    unsafe { RegCloseKey(key) };
    value
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn file_mirror_round_trip() {
        let dir = std::env::temp_dir().join(format!("verosupd-cfg-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let mut settings = defaults(&dir);
        settings.install_id = "11111111-2222-4333-8444-555555555555".into();
        settings.bootstrap_url = "https://boot.example".into();
        settings.update_url = "https://upd.example".into();
        settings.retention_days = 30;
        save_settings(&settings).unwrap();
        let loaded = load_file(&dir).unwrap();
        assert_eq!(loaded.bootstrap_url, settings.bootstrap_url);
        assert_eq!(loaded.retention_days, 30);
        let (boot, upd) = loaded.origins();
        assert_eq!(boot, "https://boot.example");
        assert_eq!(upd, "https://upd.example");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn empty_urls_use_placeholders() {
        let settings = defaults(Path::new("state"));
        let (boot, upd) = settings.origins();
        assert_eq!(boot, DEFAULT_BOOTSTRAP_URL);
        assert_eq!(upd, DEFAULT_UPDATE_URL);
    }
}
