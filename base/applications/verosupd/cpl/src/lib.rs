/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/cpl/src/lib.rs
 * PURPOSE:     VerOS Update control panel applet
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

use std::ffi::c_void;
use verosupdsvc::{load_settings, save_settings, PackageDb};
use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, GetMessageW, GetWindowTextW,
    MessageBoxW, PostQuitMessage, RegisterClassW, SendMessageW, SetWindowTextW, TranslateMessage,
    BS_PUSHBUTTON, CS_HREDRAW, CS_VREDRAW, CW_USEDEFAULT, ES_NUMBER, MB_OK, MSG, WM_COMMAND,
    WM_DESTROY, WNDCLASSW, WS_CHILD, WS_OVERLAPPEDWINDOW, WS_VISIBLE, WS_TABSTOP, WS_BORDER,
};

const CPL_INIT: usize = 1;
const CPL_GETCOUNT: usize = 2;
const CPL_INQUIRE: usize = 3;
const CPL_DBLCLK: usize = 5;
const CPL_STOP: usize = 6;
const CPL_EXIT: usize = 7;
const CPL_NEWINQUIRE: usize = 8;

const IDC_RETENTION: i32 = 101;
const IDC_SAVE: i32 = 102;
const IDC_CHECK: i32 = 103;

#[no_mangle]
pub extern "system" fn CPlApplet(hwnd: HWND, msg: u32, lparam1: LPARAM, _lparam2: LPARAM) -> LRESULT {
    match msg as usize {
        CPL_INIT | CPL_STOP | CPL_EXIT | CPL_INQUIRE | CPL_NEWINQUIRE => 1,
        CPL_GETCOUNT => 1,
        CPL_DBLCLK => {
            if lparam1 == 0 {
                show_panel(hwnd);
            }
            0
        }
        _ => 0,
    }
}

fn show_panel(parent: HWND) {
    let class_name: Vec<u16> = "VerOSUpdatePanel".encode_utf16().chain(std::iter::once(0)).collect();
    let class = WNDCLASSW {
        style: CS_HREDRAW | CS_VREDRAW,
        lpfnWndProc: Some(panel_proc),
        cbClsExtra: 0,
        cbWndExtra: 0,
        hInstance: std::ptr::null_mut(),
        hIcon: std::ptr::null_mut(),
        hCursor: std::ptr::null_mut(),
        hbrBackground: std::ptr::null_mut(),
        lpszMenuName: std::ptr::null(),
        lpszClassName: class_name.as_ptr(),
    };
    unsafe {
        RegisterClassW(&class);
        let title: Vec<u16> = "VerOS Update".encode_utf16().chain(std::iter::once(0)).collect();
        let hwnd = CreateWindowExW(
            0,
            class_name.as_ptr(),
            title.as_ptr(),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            520,
            360,
            parent,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        );
        if hwnd.is_null() {
            return;
        }
        let mut message = MSG {
            hwnd: std::ptr::null_mut(),
            message: 0,
            wParam: 0,
            lParam: 0,
            time: 0,
            pt: windows_sys::Win32::Foundation::POINT { x: 0, y: 0 },
        };
        while GetMessageW(&mut message, std::ptr::null_mut(), 0, 0) > 0 {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

unsafe extern "system" fn panel_proc(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    match msg {
        1 => { // WM_CREATE
            build_controls(hwnd);
            0
        }
        WM_COMMAND => {
            let id = (wparam & 0xffff) as i32;
            if id == IDC_SAVE {
                save_retention(hwnd);
            } else if id == IDC_CHECK {
                let settings = load_settings();
                let _ = verosupdsvc::check_in(&settings, true);
            }
            0
        }
        WM_DESTROY => {
            PostQuitMessage(0);
            0
        }
        _ => DefWindowProcW(hwnd, msg, wparam, lparam),
    }
}

unsafe fn build_controls(hwnd: HWND) {
    let settings = load_settings();
    let (bootstrap, update) = settings.origins();
    let packages = PackageDb::open(&settings.state_dir)
        .ok()
        .and_then(|db| db.list().ok())
        .map(|items| {
            items
                .into_iter()
                .map(|item| format!("{} {}", item.name, item.version))
                .collect::<Vec<_>>()
                .join(", ")
        })
        .unwrap_or_else(|| "no packages".into());
    let summary = format!(
        "Bootstrap: {bootstrap}\r\nUpdate: {update}\r\nLicense: {}\r\nPackages: {packages}",
        if settings.license_key.is_empty() { "not set" } else { "set" }
    );
    static_text(hwnd, &summary, 12, 12, 480, 160);
    static_text(hwnd, "Rollback retention days", 12, 190, 200, 24);
    let edit = child(
        hwnd,
        "EDIT",
        &settings.retention_days.to_string(),
        WS_CHILD | WS_VISIBLE | WS_BORDER | (ES_NUMBER as u32) | WS_TABSTOP,
        220,
        188,
        80,
        24,
        IDC_RETENTION,
    );
    let _ = edit;
    child(hwnd, "BUTTON", "Save retention", WS_CHILD | WS_VISIBLE | (BS_PUSHBUTTON as u32) | WS_TABSTOP, 12, 230, 140, 28, IDC_SAVE);
    child(hwnd, "BUTTON", "Check now", WS_CHILD | WS_VISIBLE | (BS_PUSHBUTTON as u32) | WS_TABSTOP, 164, 230, 140, 28, IDC_CHECK);
}

unsafe fn static_text(parent: HWND, text: &str, x: i32, y: i32, w: i32, h: i32) {
    child(parent, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, 0);
}

unsafe fn child(parent: HWND, class: &str, text: &str, style: u32, x: i32, y: i32, w: i32, h: i32, id: i32) -> HWND {
    let class: Vec<u16> = class.encode_utf16().chain(std::iter::once(0)).collect();
    let text: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
    CreateWindowExW(
        0,
        class.as_ptr(),
        text.as_ptr(),
        style,
        x,
        y,
        w,
        h,
        parent,
        id as *mut c_void,
        std::ptr::null_mut(),
        std::ptr::null_mut(),
    )
}

unsafe fn save_retention(hwnd: HWND) {
    let edit = windows_sys::Win32::UI::WindowsAndMessaging::GetDlgItem(hwnd, IDC_RETENTION);
    let mut buf = [0u16; 16];
    let len = GetWindowTextW(edit, buf.as_mut_ptr(), buf.len() as i32);
    let text = String::from_utf16_lossy(&buf[..len.max(0) as usize]);
    if let Ok(days) = text.parse::<u32>() {
        let mut settings = load_settings();
        settings.retention_days = verosupdsvc::clamp_retention(days);
        if save_settings(&settings).is_ok() {
            tell(hwnd, "Retention saved");
            return;
        }
    }
    tell(hwnd, "Retention must be 1 to 365 days");
}

unsafe fn tell(hwnd: HWND, text: &str) {
    let text: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
    let title: Vec<u16> = "VerOS Update".encode_utf16().chain(std::iter::once(0)).collect();
    MessageBoxW(hwnd, text.as_ptr(), title.as_ptr(), MB_OK);
}

pub use verosupdsvc::clamp_retention;

// Silence unused imports when the dialog path is type-checked without a message loop call.
#[allow(dead_code)]
fn _touch() {
    let _ = (SetWindowTextW, SendMessageW, DestroyWindow);
}
