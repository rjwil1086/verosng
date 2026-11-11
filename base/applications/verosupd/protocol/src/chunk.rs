/*
 * PROJECT:     VerOS NT
 * LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
 * FILE:        base/applications/verosupd/protocol/src/chunk.rs
 * PURPOSE:     Content-defined chunks for reverse-diff storage
 * COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@verosnt.com>
 * PROGRAMMERS: Ryan Wilson
 */

pub const MIN_CHUNK: usize = 2048;
pub const AVG_CHUNK: usize = 8192;
pub const MAX_CHUNK: usize = 16_384;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Chunk {
    pub offset: usize,
    pub len: usize,
}

pub fn chunk_bytes(data: &[u8]) -> Vec<Chunk> {
    if data.is_empty() {
        return Vec::new();
    }
    let mut out = Vec::new();
    let mut start = 0;
    while start < data.len() {
        let remaining = data.len() - start;
        if remaining <= MIN_CHUNK {
            out.push(Chunk { offset: start, len: remaining });
            break;
        }
        let search_end = (start + MAX_CHUNK).min(data.len());
        let mut split = search_end;
        let mut hash = 0u64;
        for (index, byte) in data[start..search_end].iter().copied().enumerate() {
            hash = hash.rotate_left(1) ^ GEAR[byte as usize];
            if index + 1 >= MIN_CHUNK && hash & (AVG_CHUNK as u64 - 1) == 0 {
                split = start + index + 1;
                break;
            }
        }
        out.push(Chunk {
            offset: start,
            len: split - start,
        });
        start = split;
    }
    out
}

const GEAR: [u64; 256] = {
    let mut table = [0u64; 256];
    let mut state = 0x9E37_79B9_7F4A_7C15u64;
    let mut i = 0;
    while i < 256 {
        state = state.wrapping_mul(0x6C62_272E_07BB_0142).wrapping_add(1);
        table[i] = state;
        i += 1;
    }
    table
};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identical_regions_share_chunk_edges() {
        let mut first = vec![7u8; 50_000];
        let mut second = first.clone();
        first.extend_from_slice(b"alpha-unique-tail-aaaaaaaa");
        second.extend_from_slice(b"beta-unique-tail-bbbbbbbbb");
        let a = chunk_bytes(&first);
        let b = chunk_bytes(&second);
        let shared = a.iter().zip(b.iter()).take_while(|(l, r)| l == r).count();
        assert!(shared >= 2, "shared={shared}");
    }
}
