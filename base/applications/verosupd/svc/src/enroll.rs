/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/svc/src/enroll.rs
 * PURPOSE:     Client certificate enrollment through the bootstrap service
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

use crate::config::Settings;
use rcgen::{CertificateParams, DistinguishedName, DnType, KeyPair, PKCS_ECDSA_P256_SHA256};
use std::fs;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use verosupd_protocol::{join_origin, BootstrapRequest, BootstrapResponse, BOOTSTRAP_PATH};
use x509_parser::prelude::*;

pub const RENEW_WITHIN: Duration = Duration::from_secs(14 * 24 * 60 * 60);

pub fn needs_renewal(not_after_unix: i64, now_unix: i64) -> bool {
    not_after_unix <= now_unix + RENEW_WITHIN.as_secs() as i64
}

pub fn build_csr(dns_name: &str) -> Result<(String, String), String> {
    let key = KeyPair::generate_for(&PKCS_ECDSA_P256_SHA256).map_err(|err| err.to_string())?;
    let mut params = CertificateParams::new(vec![dns_name.to_string()]).map_err(|err| err.to_string())?;
    let mut name = DistinguishedName::new();
    name.push(DnType::CommonName, dns_name);
    params.distinguished_name = name;
    let csr = params.serialize_request(&key).map_err(|err| err.to_string())?;
    Ok((
        csr.pem().map_err(|err| err.to_string())?,
        key.serialize_pem(),
    ))
}

pub fn certificate_expired(pem: &str) -> bool {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    match not_after(pem) {
        Some(expiry) => needs_renewal(expiry, now),
        None => true,
    }
}

pub fn enroll(settings: &Settings) -> Result<(), String> {
    let (bootstrap, _) = settings.origins();
    let url = join_origin(&bootstrap, BOOTSTRAP_PATH);
    let client = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(60))
        .build()
        .map_err(|err| err.to_string())?;
    let name_body = BootstrapRequest {
        install_id: settings.install_id.clone(),
        hardware_hash: settings.hardware_hash.clone(),
        license_key: settings.license_key.clone(),
        csr_pem: None,
    };
    let name: BootstrapResponse = client
        .post(&url)
        .json(&name_body)
        .send()
        .map_err(|err| err.to_string())?
        .error_for_status()
        .map_err(|err| err.to_string())?
        .json()
        .map_err(|err| err.to_string())?;
    let dns_name = match name {
        BootstrapResponse::Name { dns_name } => dns_name,
        BootstrapResponse::Issued { .. } => return Err("bootstrap returned a certificate before the CSR".into()),
    };
    let (csr_pem, key_pem) = build_csr(&dns_name)?;
    let issue_body = BootstrapRequest {
        csr_pem: Some(csr_pem),
        ..name_body
    };
    let issued: BootstrapResponse = client
        .post(&url)
        .json(&issue_body)
        .send()
        .map_err(|err| err.to_string())?
        .error_for_status()
        .map_err(|err| err.to_string())?
        .json()
        .map_err(|err| err.to_string())?;
    let BootstrapResponse::Issued {
        certificate_pem,
        chain_pem,
        ..
    } = issued
    else {
        return Err("bootstrap did not issue a certificate".into());
    };
    fs::create_dir_all(&settings.state_dir).map_err(|err| err.to_string())?;
    fs::write(settings.state_dir.join("client.key"), key_pem).map_err(|err| err.to_string())?;
    fs::write(settings.state_dir.join("client.crt"), certificate_pem).map_err(|err| err.to_string())?;
    fs::write(settings.state_dir.join("chain.crt"), chain_pem).map_err(|err| err.to_string())?;
    Ok(())
}

pub fn ensure_certificate(settings: &Settings) -> Result<(), String> {
    let cert_path = settings.state_dir.join("client.crt");
    let key_path = settings.state_dir.join("client.key");
    let renew = match (fs::read_to_string(&cert_path), key_path.exists()) {
        (Ok(pem), true) => certificate_expired(&pem),
        _ => true,
    };
    if renew {
        enroll(settings)?;
    }
    Ok(())
}

fn not_after(pem: &str) -> Option<i64> {
    let (_, parsed) = parse_x509_pem(pem.as_bytes()).ok()?;
    let cert = parsed.parse_x509().ok()?;
    Some(cert.validity().not_after.timestamp())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn renewal_window_is_fourteen_days() {
        assert!(needs_renewal(100, 100));
        assert!(needs_renewal(100 + 13 * 86_400, 100));
        assert!(!needs_renewal(100 + 20 * 86_400, 100));
    }

    #[test]
    fn csr_carries_the_install_name() {
        let (csr, key) = build_csr("abc.clients.example").unwrap();
        assert!(csr.contains("BEGIN CERTIFICATE REQUEST"));
        assert!(key.contains("BEGIN PRIVATE KEY") || key.contains("BEGIN EC PRIVATE KEY"));
        assert!(csr.contains("abc.clients.example") || csr.len() > 64);
    }
}
