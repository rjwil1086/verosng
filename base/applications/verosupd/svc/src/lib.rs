/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/svc/src/lib.rs
 * PURPOSE:     Update service library used by the daemon, CLI, and applet
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

mod config;
mod enroll;
mod service;
mod store;
mod worker;

pub use config::{clamp_retention, load_settings, save_settings, Settings};
pub use enroll::{build_csr, needs_renewal, RENEW_WITHIN};
pub use service::{self_test, service_main};
pub use store::PackageDb;
pub use worker::{apply_downloaded, check_in, reset_database, rollback_package, WorkerError};

pub fn cli_main(args: &[String]) -> i32 {
    worker::cli(args)
}
