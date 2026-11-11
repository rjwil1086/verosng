/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/cli/src/main.rs
 * PURPOSE:     verosupd command line
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

fn main() {
    let args: Vec<String> = std::env::args().collect();
    std::process::exit(verosupdsvc::cli_main(&args));
}
