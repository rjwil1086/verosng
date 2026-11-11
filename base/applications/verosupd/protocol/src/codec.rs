/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/protocol/src/codec.rs
 * PURPOSE:     PE-aware byte delta container
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

use crate::sha256_hex;
use std::collections::HashMap;

pub const VDELTA_MAGIC: &[u8; 4] = b"VDLT";
pub const CODEC_SECTION: u32 = 1;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Codec(pub u32);

#[derive(Debug)]
pub enum DeltaError {
    BadMagic,
    Truncated,
    BadCodec,
    HashMismatch,
    BadPe,
}

struct Region {
    base_at: usize,
    base: Vec<u8>,
    next: Vec<u8>,
}

pub fn diff_delta(base: &[u8], result: &[u8]) -> Vec<u8> {
    let regions = regions(base, result);
    let mut payload = Vec::new();
    payload.extend_from_slice(&(regions.len() as u32).to_le_bytes());
    for region in &regions {
        payload.extend_from_slice(&(region.next.len() as u32).to_le_bytes());
        encode_region(&region.base, region.base_at, &region.next, &mut payload);
    }
    let compressed = zstd::encode_all(payload.as_slice(), 3).expect("zstd encode");
    let mut out = Vec::with_capacity(4 + 4 + 32 + 32 + 4 + 8 + compressed.len());
    out.extend_from_slice(VDELTA_MAGIC);
    out.extend_from_slice(&1u32.to_le_bytes());
    out.extend_from_slice(&hex_to_32(&sha256_hex(base)));
    out.extend_from_slice(&hex_to_32(&sha256_hex(result)));
    out.extend_from_slice(&CODEC_SECTION.to_le_bytes());
    out.extend_from_slice(&(compressed.len() as u64).to_le_bytes());
    out.extend_from_slice(&compressed);
    out
}

pub fn apply_delta(base: &[u8], delta: &[u8]) -> Result<Vec<u8>, DeltaError> {
    if delta.len() < 4 + 4 + 32 + 32 + 4 + 8 || &delta[0..4] != VDELTA_MAGIC {
        return Err(DeltaError::BadMagic);
    }
    let mut cursor = 4;
    let version = read_u32(delta, &mut cursor)?;
    if version != 1 {
        return Err(DeltaError::BadCodec);
    }
    let base_hash = &delta[cursor..cursor + 32];
    cursor += 32;
    let result_hash = &delta[cursor..cursor + 32];
    cursor += 32;
    let codec = read_u32(delta, &mut cursor)?;
    if codec != CODEC_SECTION {
        return Err(DeltaError::BadCodec);
    }
    if hex_to_32(&sha256_hex(base)).as_slice() != base_hash {
        return Err(DeltaError::HashMismatch);
    }
    let payload_len = read_u64(delta, &mut cursor)? as usize;
    if cursor + payload_len != delta.len() {
        return Err(DeltaError::Truncated);
    }
    let payload = zstd::decode_all(&delta[cursor..]).map_err(|_| DeltaError::Truncated)?;
    let mut pos = 0;
    let count = read_u32(&payload, &mut pos)? as usize;
    let mut result = Vec::new();
    for _ in 0..count {
        let expected = read_u32(&payload, &mut pos)? as usize;
        let start = result.len();
        decode_region(base, &payload, &mut pos, &mut result)?;
        if result.len() - start != expected {
            return Err(DeltaError::Truncated);
        }
    }
    if hex_to_32(&sha256_hex(&result)).as_slice() != result_hash {
        return Err(DeltaError::HashMismatch);
    }
    Ok(result)
}

fn regions(base: &[u8], result: &[u8]) -> Vec<Region> {
    let base_ranges = cover(base);
    let next_ranges = cover(result);
    if base_ranges.len() == next_ranges.len() {
        base_ranges
            .into_iter()
            .zip(next_ranges)
            .map(|(base_range, next_range)| Region {
                base_at: base_range.0,
                base: base[base_range.0..base_range.1].to_vec(),
                next: result[next_range.0..next_range.1].to_vec(),
            })
            .collect()
    } else {
        vec![Region {
            base_at: 0,
            base: base.to_vec(),
            next: result.to_vec(),
        }]
    }
}

fn cover(file: &[u8]) -> Vec<(usize, usize)> {
    let ranges = pe_ranges(file).unwrap_or_default();
    let mut cursor = 0;
    let mut out = Vec::new();
    for (start, end) in ranges {
        if start > cursor {
            out.push((cursor, start));
        }
        let from = start.max(cursor);
        if end > from {
            out.push((from, end));
            cursor = end;
        }
    }
    if cursor < file.len() {
        out.push((cursor, file.len()));
    }
    if out.is_empty() {
        out.push((0, file.len()));
    }
    out
}

fn pe_ranges(file: &[u8]) -> Result<Vec<(usize, usize)>, DeltaError> {
    if file.len() < 0x40 || &file[0..2] != b"MZ" {
        return Err(DeltaError::BadPe);
    }
    let lfanew = u32::from_le_bytes(file[0x3C..0x40].try_into().unwrap()) as usize;
    if lfanew + 24 > file.len() || &file[lfanew..lfanew + 4] != b"PE\0\0" {
        return Err(DeltaError::BadPe);
    }
    let sections = u16::from_le_bytes(file[lfanew + 6..lfanew + 8].try_into().unwrap()) as usize;
    let optional = u16::from_le_bytes(file[lfanew + 20..lfanew + 22].try_into().unwrap()) as usize;
    let table = lfanew + 24 + optional;
    if sections == 0 || table + sections * 40 > file.len() {
        return Err(DeltaError::BadPe);
    }
    let mut ranges = vec![(0, table)];
    for index in 0..sections {
        let entry = table + index * 40;
        let raw_size = u32::from_le_bytes(file[entry + 16..entry + 20].try_into().unwrap()) as usize;
        let raw_ptr = u32::from_le_bytes(file[entry + 20..entry + 24].try_into().unwrap()) as usize;
        if raw_size == 0 {
            continue;
        }
        if raw_ptr.saturating_add(raw_size) > file.len() {
            return Err(DeltaError::BadPe);
        }
        ranges.push((raw_ptr, raw_ptr + raw_size));
    }
    ranges.sort_by_key(|range| range.0);
    Ok(ranges)
}

fn encode_region(base: &[u8], base_at: usize, next: &[u8], out: &mut Vec<u8>) {
    let mut index = HashMap::<[u8; 16], usize>::new();
    let mut offset = 0;
    while offset + 16 <= base.len() {
        let block = base[offset..offset + 16].try_into().unwrap();
        index.entry(block).or_insert(offset);
        offset += 16;
    }
    let mut cursor = 0;
    let mut literal: Vec<u8> = Vec::new();
    let flush_literal = |literal: &mut Vec<u8>, out: &mut Vec<u8>| {
        if literal.is_empty() {
            return;
        }
        out.push(1);
        out.extend_from_slice(&(literal.len() as u32).to_le_bytes());
        out.extend_from_slice(literal);
        literal.clear();
    };
    while cursor < next.len() {
        let mut matched = None;
        if cursor + 16 <= next.len() {
            let block: [u8; 16] = next[cursor..cursor + 16].try_into().unwrap();
            if let Some(at) = index.get(&block).copied() {
                let mut length = 16;
                while cursor + length < next.len()
                    && at + length < base.len()
                    && next[cursor + length] == base[at + length]
                {
                    length += 1;
                }
                matched = Some((base_at + at, length));
            }
        }
        if let Some((at, length)) = matched {
            flush_literal(&mut literal, out);
            out.push(2);
            out.extend_from_slice(&(at as u32).to_le_bytes());
            out.extend_from_slice(&(length as u32).to_le_bytes());
            cursor += length;
        } else {
            literal.push(next[cursor]);
            cursor += 1;
        }
    }
    flush_literal(&mut literal, out);
    out.push(0);
}

fn decode_region(base: &[u8], payload: &[u8], pos: &mut usize, out: &mut Vec<u8>) -> Result<(), DeltaError> {
    loop {
        if *pos >= payload.len() {
            return Err(DeltaError::Truncated);
        }
        let tag = payload[*pos];
        *pos += 1;
        match tag {
            0 => return Ok(()),
            1 => {
                let len = read_u32(payload, pos)? as usize;
                if *pos + len > payload.len() {
                    return Err(DeltaError::Truncated);
                }
                out.extend_from_slice(&payload[*pos..*pos + len]);
                *pos += len;
            }
            2 => {
                let at = read_u32(payload, pos)? as usize;
                let len = read_u32(payload, pos)? as usize;
                if at + len > base.len() {
                    return Err(DeltaError::Truncated);
                }
                out.extend_from_slice(&base[at..at + len]);
            }
            _ => return Err(DeltaError::BadCodec),
        }
    }
}

fn read_u32(bytes: &[u8], pos: &mut usize) -> Result<u32, DeltaError> {
    if *pos + 4 > bytes.len() {
        return Err(DeltaError::Truncated);
    }
    let value = u32::from_le_bytes(bytes[*pos..*pos + 4].try_into().unwrap());
    *pos += 4;
    Ok(value)
}

fn read_u64(bytes: &[u8], pos: &mut usize) -> Result<u64, DeltaError> {
    if *pos + 8 > bytes.len() {
        return Err(DeltaError::Truncated);
    }
    let value = u64::from_le_bytes(bytes[*pos..*pos + 8].try_into().unwrap());
    *pos += 8;
    Ok(value)
}

fn hex_to_32(hex_text: &str) -> [u8; 32] {
    let bytes = hex::decode(hex_text).expect("sha256 hex");
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    out
}

pub fn tiny_pe(section: &[u8]) -> Vec<u8> {
    let mut file = vec![0u8; 0x200];
    file[0] = b'M';
    file[1] = b'Z';
    file[0x3C] = 0x80;
    file[0x80] = b'P';
    file[0x81] = b'E';
    file[0x86] = 1; // one section
    file[0x94] = 0xF0; // size of optional header, so the section table starts at 0x80+24+0xF0 = 0x188
    let entry = 0x188;
    file[entry + 16..entry + 20].copy_from_slice(&(section.len() as u32).to_le_bytes());
    file[entry + 20..entry + 24].copy_from_slice(&0x200u32.to_le_bytes());
    file.extend_from_slice(section);
    file
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_changes_one_section() {
        let base = tiny_pe(b"abcdefghijklmnopqrstuvwxyz012345");
        let mut result = tiny_pe(b"abcdefghijklmnopqrstuvwxyz012345");
        let at = result.len() - 8;
        result[at..at + 4].copy_from_slice(b"WXYZ");
        let delta = diff_delta(&base, &result);
        let applied = apply_delta(&base, &delta).unwrap();
        assert_eq!(applied, result);
        assert!(delta.len() < result.len());
    }

    #[test]
    fn rejects_a_different_base() {
        let base = tiny_pe(b"abcdefghijklmnopqrstuvwxyz012345");
        let result = tiny_pe(b"abcdefghijklmnopqrstuvwxyzABCDEF");
        let delta = diff_delta(&base, &result);
        let other = tiny_pe(b"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz");
        assert!(apply_delta(&other, &delta).is_err());
    }
}
