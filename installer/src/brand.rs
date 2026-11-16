//! The port's look shared by the setup window and the launcher: the app icon, the "II Reborn"
//! wolf emblem (native/assets/fable2-reborn-logo.png). The executables get the same art as a
//! Windows icon resource (assets/fable2.ico, embedded by build.rs).
//!
//! assets/icon_<n>.rgba are straight-alpha RGBA pixels made from that art: 16-32 px are a round
//! crop of the medallion (wolf head and "II", readable in the taskbar), 40 px and up the full
//! emblem with the "Reborn" ribbon.

const SIZES: &[(usize, &[u8])] = &[
    (16, include_bytes!("../assets/icon_16.rgba")),
    (20, include_bytes!("../assets/icon_20.rgba")),
    (24, include_bytes!("../assets/icon_24.rgba")),
    (32, include_bytes!("../assets/icon_32.rgba")),
    (40, include_bytes!("../assets/icon_40.rgba")),
    (48, include_bytes!("../assets/icon_48.rgba")),
    (64, include_bytes!("../assets/icon_64.rgba")),
    (256, include_bytes!("../assets/icon_256.rgba")),
];

/// Box-filters an `s` x `s` RGBA image to `n` x `n` (alpha-weighted, so edges don't darken).
fn resample(src: &[u8], s: usize, n: usize) -> Vec<u8> {
    let mut out = vec![0u8; n * n * 4];
    for y in 0..n {
        let (y0, y1) = (y * s / n, ((y + 1) * s / n).max(y * s / n + 1).min(s));
        for x in 0..n {
            let (x0, x1) = (x * s / n, ((x + 1) * s / n).max(x * s / n + 1).min(s));
            let mut acc = [0f64; 4];
            let mut count = 0f64;
            for sy in y0..y1 {
                for sx in x0..x1 {
                    let p = (sy * s + sx) * 4;
                    let a = src[p + 3] as f64;
                    acc[0] += src[p] as f64 * a;
                    acc[1] += src[p + 1] as f64 * a;
                    acc[2] += src[p + 2] as f64 * a;
                    acc[3] += a;
                    count += 1.0;
                }
            }
            let o = (y * n + x) * 4;
            if acc[3] > 0.0 {
                for c in 0..3 {
                    out[o + c] = (acc[c] / acc[3]).round().clamp(0.0, 255.0) as u8;
                }
                out[o + 3] = (acc[3] / count).round().clamp(0.0, 255.0) as u8;
            }
        }
    }
    out
}

/// The app icon, `n` x `n` straight-alpha RGBA.
pub fn icon_rgba(n: usize) -> Vec<u8> {
    if n == 0 {
        return Vec::new();
    }
    if let Some((_, px)) = SIZES.iter().find(|(s, _)| *s == n) {
        return px.to_vec();
    }
    // The smallest stored size above n (same crop as the sizes around it), else the largest.
    let (s, px) = *SIZES.iter().find(|(s, _)| *s > n).unwrap_or(SIZES.last().unwrap());
    if s >= n {
        resample(px, s, n)
    } else {
        // Larger than 256: nearest-neighbour up-scale.
        let mut out = vec![0u8; n * n * 4];
        for y in 0..n {
            for x in 0..n {
                let p = ((y * s / n) * s + x * s / n) * 4;
                out[(y * n + x) * 4..][..4].copy_from_slice(&px[p..p + 4]);
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn stored_sizes_are_complete() {
        for (s, px) in super::SIZES {
            assert_eq!(px.len(), s * s * 4, "icon_{s}.rgba");
        }
    }

    #[test]
    fn icon_has_transparent_corner_and_opaque_centre() {
        for n in [16, 32, 48, 64, 100, 256, 300] {
            let px = super::icon_rgba(n);
            assert_eq!(px.len(), n * n * 4);
            assert_eq!(px[3], 0, "{n}: corner is transparent");
            let c = (n / 2 * n + n / 2) * 4;
            assert!(px[c + 3] > 200, "{n}: centre is opaque");
        }
    }
}
