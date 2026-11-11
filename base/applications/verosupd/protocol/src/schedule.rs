/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/protocol/src/schedule.rs
 * PURPOSE:     Check-in schedule, jitter, and missed-window detection
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

use crate::JITTER_MINUTES;
use serde::{Deserialize, Serialize};
use std::time::{Duration, SystemTime};

/// Monday is bit 0. Sunday is bit 6.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
pub enum PollMode {
    Disabled,
    Manual,
    Automatic,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct Schedule {
    pub mode: PollMode,
    pub days: u8,
    pub minute_of_day: Option<u16>,
    pub interval_hours: Option<u32>,
}

impl Default for Schedule {
    fn default() -> Self {
        Self {
            mode: PollMode::Automatic,
            days: 0b111_1111,
            minute_of_day: Some(3 * 60),
            interval_hours: None,
        }
    }
}

/// Shift a scheduled instant by a deterministic offset inside ±15 minutes.
pub fn jitter_offset(seed: u64) -> Duration {
    let span = (JITTER_MINUTES * 2 + 1) as u64;
    let minutes = (seed % span) as i64 - JITTER_MINUTES;
    Duration::from_secs(minutes.unsigned_abs() * 60)
}

pub fn apply_jitter(base: SystemTime, seed: u64) -> SystemTime {
    let span = (JITTER_MINUTES * 2 + 1) as u64;
    let minutes = (seed % span) as i64 - JITTER_MINUTES;
    if minutes >= 0 {
        base + Duration::from_secs(minutes as u64 * 60)
    } else {
        base - Duration::from_secs(minutes.unsigned_abs() * 60)
    }
}

/// The latest scheduled instant at or before `now`, in whole minutes from the Unix epoch.
pub fn last_scheduled_minute(now_unix: i64, schedule: &Schedule) -> Option<i64> {
    if schedule.mode != PollMode::Automatic {
        return None;
    }
    if let Some(hours) = schedule.interval_hours {
        if hours == 0 {
            return None;
        }
        let step = hours as i64 * 60;
        return Some(now_unix / 60 / step * step);
    }
    let minute = schedule.minute_of_day? as i64;
    let day = now_unix.div_euclid(86_400);
    let minute_now = now_unix.rem_euclid(86_400) / 60;
    for back in 0..8 {
        let candidate_day = day - back;
        let weekday = (candidate_day + 3).rem_euclid(7); // 1970-01-01 was Thursday; Monday is 0
        if schedule.days & (1 << weekday) == 0 {
            continue;
        }
        if back == 0 && minute_now < minute {
            continue;
        }
        return Some(candidate_day * 24 * 60 + minute);
    }
    None
}

pub fn missed_window(now_unix: i64, last_success_unix: Option<i64>, schedule: &Schedule) -> bool {
    let Some(scheduled) = last_scheduled_minute(now_unix, schedule) else {
        return false;
    };
    match last_success_unix {
        None => true,
        Some(last) => last / 60 < scheduled,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn jitter_stays_inside_fifteen_minutes() {
        for seed in 0..64 {
            let offset = jitter_offset(seed);
            assert!(offset <= Duration::from_secs(15 * 60));
        }
    }

    #[test]
    fn missed_check_after_the_scheduled_minute() {
        let schedule = Schedule {
            mode: PollMode::Automatic,
            days: 0b111_1111,
            minute_of_day: Some(60),
            interval_hours: None,
        };
        // 1970-01-02 01:30 UTC is a Friday, after 01:00.
        let now = 86_400 + 90 * 60;
        assert!(missed_window(now, Some(86_400), &schedule));
        assert!(!missed_window(now, Some(now), &schedule));
    }

    #[test]
    fn disabled_mode_does_not_catch_up() {
        let schedule = Schedule {
            mode: PollMode::Disabled,
            days: 0b111_1111,
            minute_of_day: Some(0),
            interval_hours: None,
        };
        assert!(!missed_window(10_000, None, &schedule));
    }
}
