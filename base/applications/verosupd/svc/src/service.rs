/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/svc/src/service.rs
 * PURPOSE:     NT service control transitions for verosupdsvc
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

use std::sync::atomic::{AtomicU32, Ordering};
use windows_sys::Win32::System::Services::{
    RegisterServiceCtrlHandlerExW, SetServiceStatus, StartServiceCtrlDispatcherW, SERVICE_ACCEPT_SHUTDOWN,
    SERVICE_ACCEPT_STOP, SERVICE_CONTROL_SHUTDOWN, SERVICE_CONTROL_STOP, SERVICE_RUNNING,
    SERVICE_START_PENDING, SERVICE_STATUS, SERVICE_STATUS_HANDLE, SERVICE_STOPPED, SERVICE_STOP_PENDING,
    SERVICE_TABLE_ENTRYW, SERVICE_WIN32_OWN_PROCESS,
};

pub const START_PENDING: u32 = SERVICE_START_PENDING;
pub const RUNNING: u32 = SERVICE_RUNNING;
pub const STOP_PENDING: u32 = SERVICE_STOP_PENDING;
pub const STOPPED: u32 = SERVICE_STOPPED;

static PHASE: AtomicU32 = AtomicU32::new(STOPPED);
static STOP_FLAG: AtomicU32 = AtomicU32::new(0);

pub fn transition(control: Option<u32>) -> u32 {
    let next = match control {
        None => RUNNING,
        Some(SERVICE_CONTROL_STOP | SERVICE_CONTROL_SHUTDOWN) => STOPPED,
        Some(_) => PHASE.load(Ordering::SeqCst),
    };
    if next == STOPPED && PHASE.load(Ordering::SeqCst) == RUNNING {
        PHASE.store(STOP_PENDING, Ordering::SeqCst);
    }
    PHASE.store(next, Ordering::SeqCst);
    next
}

pub fn self_test() {
    PHASE.store(START_PENDING, Ordering::SeqCst);
    assert_eq!(transition(None), RUNNING);
    assert_eq!(transition(Some(SERVICE_CONTROL_STOP)), STOPPED);
    println!("verosupdsvc start-stop ok");
}

pub fn service_main() {
    let mut name: Vec<u16> = "verosupdsvc".encode_utf16().chain(std::iter::once(0)).collect();
    let table = [
        SERVICE_TABLE_ENTRYW {
            lpServiceName: name.as_mut_ptr(),
            lpServiceProc: Some(service_proc),
        },
        SERVICE_TABLE_ENTRYW {
            lpServiceName: std::ptr::null_mut(),
            lpServiceProc: None,
        },
    ];
    unsafe {
        StartServiceCtrlDispatcherW(table.as_ptr());
    }
}

unsafe extern "system" fn handler(control: u32, _: u32, _: *mut core::ffi::c_void, _: *mut core::ffi::c_void) -> u32 {
    if control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN {
        STOP_FLAG.store(1, Ordering::SeqCst);
        transition(Some(control));
    }
    0
}

unsafe extern "system" fn service_proc(_argc: u32, _argv: *mut *mut u16) {
    let mut name: Vec<u16> = "verosupdsvc".encode_utf16().chain(std::iter::once(0)).collect();
    let handle = RegisterServiceCtrlHandlerExW(name.as_ptr(), Some(handler), std::ptr::null_mut());
    PHASE.store(START_PENDING, Ordering::SeqCst);
    report(handle, START_PENDING);
    transition(None);
    report(handle, RUNNING);
    crate::worker::serve_until(|| STOP_FLAG.load(Ordering::SeqCst) != 0);
    PHASE.store(STOP_PENDING, Ordering::SeqCst);
    report(handle, STOP_PENDING);
    PHASE.store(STOPPED, Ordering::SeqCst);
    report(handle, STOPPED);
}

fn report(handle: SERVICE_STATUS_HANDLE, state: u32) {
    if handle.is_null() {
        return;
    }
    let mut status = SERVICE_STATUS {
        dwServiceType: SERVICE_WIN32_OWN_PROCESS,
        dwCurrentState: state,
        dwControlsAccepted: if state == RUNNING {
            SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        } else {
            0
        },
        dwWin32ExitCode: 0,
        dwServiceSpecificExitCode: 0,
        dwCheckPoint: 0,
        dwWaitHint: if state == START_PENDING || state == STOP_PENDING { 3000 } else { 0 },
    };
    unsafe {
        SetServiceStatus(handle, &mut status);
    }
}
