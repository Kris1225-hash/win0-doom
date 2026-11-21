#!/usr/bin/env python3
"""Analyse a qemu wav-audiodev capture and render waveform/spectrogram evidence.

Reproducer (on the proxmox host, after tools/audio-capture.sh):

    tools/audio-report.py \
        --wav /root/audioharness/run-*/capture.wav \
        --out-dir /root/audioharness/run-*/report

Pure python3 + numpy + PIL: parses RIFF/WAVE itself (PCM 8/16/24/32-bit,
including WAVE_FORMAT_EXTENSIBLE, mono or multichannel, any sample rate),
writes report.json (duration, rate, channels, peak/RMS dBFS, silence percent
at -60 dBFS, non-silent windows, top FFT peaks of the loudest 1 s window,
verdict), waveform.png, spectrogram.png, overview.png, and up to 12 one-second
slices of the non-silent windows in slices/window-NNN.wav.

The verdict is "audio-present" when at least one 100 ms analysis frame has an
RMS above -60 dBFS, otherwise "silence". Both cases exit 0: silence is a
measurement, not an error. A qemu capture.wav that does not exist at all is
also exit 0 with verdict "no-audio-file": qemu creates that file only when the
guest first opens the PCM voice. Every other missing input path is an error
(exit 2).

Self-test examples:

    python3 tools/audio-report.py --wav sine440.wav --out-dir out-sine
    python3 tools/audio-report.py --wav silence.wav --out-dir out-silence
"""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys

try:
    import numpy as np
except ImportError:  # pragma: no cover - host diagnostics
    sys.stderr.write("audio-report: numpy is required (python3 -m pip install numpy)\n")
    raise SystemExit(3)

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - host diagnostics
    sys.stderr.write("audio-report: Pillow is required (python3 -m pip install Pillow)\n")
    raise SystemExit(3)


BG = (13, 17, 23)
PANEL = (22, 27, 34)
GRID = (38, 48, 60)
AXIS = (74, 88, 104)
TEXT = (201, 209, 217)
MUTED = (139, 148, 158)
WAVE = (95, 215, 255)
ACCENT = (255, 166, 87)
GOOD = (126, 231, 135)
BAD = (255, 123, 114)

DB_FLOOR = -120.0
SILENCE_DBFS = -60.0
FRAME_SECONDS = 0.1
MERGE_GAP_SECONDS = 0.25

COLOR_STOPS = [
    (0.00, (8, 8, 24)),
    (0.20, (35, 15, 70)),
    (0.40, (90, 20, 110)),
    (0.60, (180, 50, 90)),
    (0.80, (245, 130, 40)),
    (1.00, (255, 240, 180)),
]


class WavError(Exception):
    pass


def build_colormap(count=256):
    lut = np.zeros((count, 3), dtype=np.uint8)
    positions = np.linspace(0.0, 1.0, count)
    for index, value in enumerate(positions):
        for stop in range(len(COLOR_STOPS) - 1):
            left_pos, left_color = COLOR_STOPS[stop]
            right_pos, right_color = COLOR_STOPS[stop + 1]
            if left_pos <= value <= right_pos:
                span = right_pos - left_pos or 1.0
                blend = (value - left_pos) / span
                lut[index] = [
                    int(round(left_color[channel] * (1.0 - blend) +
                              right_color[channel] * blend))
                    for channel in range(3)
                ]
                break
    return lut


LUT = build_colormap()


def load_font(size, bold=False):
    names = (
        ["DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"]
        if bold
        else ["DejaVuSans.ttf", "LiberationSans-Regular.ttf"]
    )
    directories = [
        "/usr/share/fonts/truetype/dejavu",
        "/usr/share/fonts/TTF",
        "/usr/share/fonts/truetype/liberation",
        "/usr/share/fonts",
        "/System/Library/Fonts/Supplemental",
        "/Library/Fonts",
        os.path.expanduser("~/Library/Fonts"),
    ]
    for directory in directories:
        for name in names:
            path = os.path.join(directory, name)
            if os.path.isfile(path):
                try:
                    return ImageFont.truetype(path, size)
                except OSError:
                    continue
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def draw_vertical_text(image, text, font, fill, x, centre_y):
    """Draw text rotated to read bottom-to-top in the left margin."""
    bbox = font.getbbox(text)
    width = bbox[2] - bbox[0] + 4
    height = bbox[3] - bbox[1] + 4
    layer = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((2 - bbox[0], 2 - bbox[1]), text,
                               font=font, fill=fill)
    layer = layer.rotate(90, expand=True)
    image.paste(layer, (x, int(centre_y - layer.height / 2)), layer)


def to_db(value):
    if value is None or not math.isfinite(value) or value <= 0.0:
        return DB_FLOOR
    return max(DB_FLOOR, 20.0 * math.log10(value))


def array_db(values):
    return 20.0 * np.log10(np.maximum(values, 1e-12))


def nice_step(duration, target=8):
    candidates = [0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5, 10,
                  15, 20, 30, 60, 120, 300]
    if duration <= 0:
        return 1.0
    ideal = duration / max(target, 1)
    for candidate in candidates:
        if candidate >= ideal:
            return float(candidate)
    return float(candidates[-1])


def fmt_hz(value):
    if value >= 1000:
        return f"{value / 1000:g}k"
    return f"{value:g}"


def fmt_time(value):
    if value >= 60:
        return f"{value / 60:.1f}m"
    return f"{value:g}s"


# ---------------------------------------------------------------------------
# RIFF/WAVE parsing


def parse_fmt(chunk):
    if len(chunk) < 16:
        raise WavError("fmt chunk is shorter than 16 bytes")
    tag, channels, rate, byte_rate, block_align, bits = struct.unpack_from(
        "<HHIIHH", chunk, 0
    )
    if tag == 0xFFFE:
        if len(chunk) < 40:
            raise WavError("extensible fmt chunk is shorter than 40 bytes")
        subformat = chunk[24:40]
        if subformat[:2] != b"\x01\x00":
            raise WavError("WAVE_FORMAT_EXTENSIBLE subformat is not PCM")
        tag = 1
    if tag not in (1, 3):
        raise WavError(f"unsupported WAVE format tag {tag} (need PCM)")
    if channels < 1 or channels > 64:
        raise WavError(f"implausible channel count {channels}")
    if rate < 1 or rate > 4_000_000:
        raise WavError(f"implausible sample rate {rate}")
    if bits not in (8, 16, 24, 32):
        raise WavError(f"unsupported sample width {bits} bits")
    if tag == 3 and bits != 32:
        raise WavError(f"unsupported float sample width {bits} bits")
    if block_align != channels * (bits // 8):
        raise WavError("inconsistent block alignment")
    return {
        "tag": tag,
        "channels": channels,
        "rate": rate,
        "byte_rate": byte_rate,
        "block_align": block_align,
        "bits": bits,
    }


def parse_wav(path):
    with open(path, "rb") as handle:
        raw = handle.read()
    if len(raw) < 12:
        raise WavError(f"{path} is not a RIFF/WAVE file")
    if raw[4:8] == b"RF64":
        raise WavError("RF64 is not supported")
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise WavError(f"{path} is not a RIFF/WAVE file")
    declared_riff = struct.unpack_from("<I", raw, 4)[0]
    position = 12
    fmt = None
    payload = None
    data_offset = None
    data_declared = None
    data_derived = False
    while position + 8 <= len(raw):
        chunk_id = raw[position:position + 4]
        size = struct.unpack_from("<I", raw, position + 4)[0]
        body = position + 8
        remaining = len(raw) - body
        if chunk_id == b"data":
            # qemu's wav audiodev leaves RIFF and data sizes zeroed when the
            # process shuts down without running wav_fini_out().  Do not
            # trust those fields: derive the payload length from the file.
            data_offset = body
            data_declared = size
            if size == 0 or size > remaining:
                size = remaining
                data_derived = True
            payload = raw[body:body + size]
            break
        if size > remaining:
            raise WavError(f"chunk {chunk_id!r} size {size} exceeds file")
        chunk = raw[body:body + size]
        if chunk_id == b"fmt ":
            fmt = parse_fmt(chunk)
        position = body + size + (size & 1)
    if fmt is None:
        raise WavError("no fmt chunk")
    if payload is None:
        raise WavError("no data chunk")
    block = fmt["block_align"]
    usable = len(payload) - (len(payload) % block)
    payload = payload[:usable]
    frames = len(payload) // block
    if frames == 0:
        samples = np.zeros((0, fmt["channels"]), dtype=np.float32)
    else:
        samples = decode_payload(payload, fmt)
    riff_info = {
        "riff_size_declared": declared_riff,
        "riff_size_valid": bool(declared_riff and declared_riff + 8 <= len(raw)),
        "data_offset": data_offset,
        "data_size_declared": data_declared,
        "data_size_used": len(payload),
        "data_size_derived": data_derived,
    }
    return fmt, payload, samples, riff_info


def decode_payload(payload, fmt):
    bits = fmt["bits"]
    channels = fmt["channels"]
    if bits == 8:
        values = np.frombuffer(payload, dtype=np.uint8).astype(np.float32)
        values = (values - 128.0) / 128.0
    elif bits == 16:
        values = np.frombuffer(payload, dtype="<i2").astype(np.float32) / 32768.0
    elif bits == 24:
        octets = np.frombuffer(payload, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        values = octets[:, 0] | (octets[:, 1] << 8) | (octets[:, 2] << 16)
        values = np.where(values >= (1 << 23), values - (1 << 24), values)
        values = values.astype(np.float32) / 8388608.0
    else:
        if fmt["tag"] == 3:
            values = np.frombuffer(payload, dtype="<f4").astype(np.float32)
        else:
            values = np.frombuffer(payload, dtype="<i4").astype(np.float32) / 2147483648.0
    return values.reshape(-1, channels)


# ---------------------------------------------------------------------------
# analysis


def reference_signal(samples):
    """Mono signal for the waveform/STFT that does not cancel antiphase stereo.

    A plain channel mean turns L=+sine, R=-sine into silence.  Use the mean
    when it carries most of the strongest channel's energy, otherwise fall
    back to that channel.
    """
    if samples.shape[0] == 0:
        return np.zeros(0, dtype=np.float32)
    if samples.shape[1] == 1:
        return samples[:, 0].astype(np.float32)
    mean_signal = samples.mean(axis=1)
    channel_rms = np.sqrt(np.mean(np.square(samples, dtype=np.float64), axis=0))
    strongest = float(channel_rms.max()) if channel_rms.size else 0.0
    mean_rms = float(np.sqrt(np.mean(np.square(mean_signal, dtype=np.float64))))
    if strongest and mean_rms < 0.5 * strongest:
        return samples[:, int(np.argmax(channel_rms))].astype(np.float32)
    return mean_signal.astype(np.float32)


def analyse(samples, rate, channels):
    frames = samples.shape[0]
    duration = frames / rate if rate else 0.0
    mono = reference_signal(samples)

    if frames:
        peak = float(np.max(np.abs(samples)))
        rms = float(np.sqrt(np.mean(np.square(samples, dtype=np.float64))))
    else:
        peak = 0.0
        rms = 0.0

    frame_len = max(1, int(round(rate * FRAME_SECONDS)))
    if frames:
        # Keep the final partial frame: a capture can end mid-frame, and
        # dropping it would hide a short burst and bias silence_percent.
        frame_count = (frames + frame_len - 1) // frame_len
        frame_start = np.arange(frame_count, dtype=np.int64) * frame_len
        counts = np.diff(np.append(frame_start, frames)).astype(np.float64)
        flat = samples.reshape(-1)
        starts_flat = frame_start * channels
        sums = np.add.reduceat(np.square(flat, dtype=np.float64), starts_flat)
        frame_rms = np.sqrt(sums / (counts * channels))
    else:
        frame_rms = np.zeros(0, dtype=np.float64)
        frame_start = np.zeros(0, dtype=np.int64)

    frame_db = array_db(frame_rms)
    mask = frame_db > SILENCE_DBFS
    total_frames = int(frame_rms.size)
    if total_frames:
        silence_percent = 100.0 * (1.0 - float(np.count_nonzero(mask)) / total_frames)
    else:
        silence_percent = 100.0

    runs = []
    for index in np.flatnonzero(mask):
        index = int(index)
        if runs and index == runs[-1][1] + 1:
            runs[-1][1] = index
        else:
            runs.append([index, index])
    merged = []
    for start, end in runs:
        if merged and (start - merged[-1][1] - 1) * FRAME_SECONDS <= MERGE_GAP_SECONDS:
            merged[-1][1] = end
        else:
            merged.append([start, end])

    windows = []
    for start, end in merged:
        begin_sample = int(frame_start[start])
        end_sample = int(min(frames, frame_start[end] + frame_len))
        if end_sample <= begin_sample:
            continue
        window_samples = samples[begin_sample:end_sample]
        window = mono[begin_sample:end_sample]
        window_rms = float(np.sqrt(np.mean(
            np.square(window_samples, dtype=np.float64)
        )))
        window_peak = float(np.max(np.abs(window_samples)))
        duration_window = (end_sample - begin_sample) / rate
        peak_offset = int(np.argmax(np.abs(window))) if window.size else 0
        windows.append({
            "start_s": round(begin_sample / rate, 4),
            "end_s": round(end_sample / rate, 4),
            "duration_s": round(duration_window, 4),
            "rms_dbfs": round(to_db(window_rms), 2),
            "peak_dbfs": round(to_db(window_peak), 2),
            "_begin_sample": begin_sample,
            "_end_sample": end_sample,
            "_peak_sample": begin_sample + peak_offset,
            "_rms": window_rms,
        })

    # Top five FFT peaks of the loudest one-second window, using all channels
    # so antiphase stereo does not cancel.
    segment_len = min(frames, max(1, int(round(rate))))
    if frames:
        if frames <= segment_len:
            segment_start = 0
        else:
            energy = np.sum(np.square(samples, dtype=np.float64), axis=1)
            cumulative = np.concatenate(([0.0], np.cumsum(energy)))
            sums = cumulative[segment_len:] - cumulative[:-segment_len]
            segment_start = int(np.argmax(sums))
        segment = samples[segment_start:segment_start + segment_len].astype(np.float64)
        segment_end = segment_start + segment_len
    else:
        segment_start = segment_end = 0
        segment = np.zeros((0, channels), dtype=np.float64)

    peaks = []
    if segment.shape[0] >= 2:
        window_fn = np.hanning(segment.shape[0])
        spectra = np.abs(np.fft.rfft(segment * window_fn[:, None], axis=0))
        normalisation = 2.0 / float(window_fn.sum()) if window_fn.sum() else 1.0
        amplitude = spectra.mean(axis=1) * normalisation
        frequencies = np.fft.rfftfreq(segment.shape[0], 1.0 / rate)
        valid = frequencies >= 20.0
        amplitude = amplitude[valid]
        frequencies = frequencies[valid]
        if amplitude.size and float(amplitude.max()) > 1e-7:
            local = np.flatnonzero(
                (amplitude[1:-1] >= amplitude[:-2]) &
                (amplitude[1:-1] >= amplitude[2:])
            ) + 1
            if local.size:
                strongest = float(amplitude[local].max())
                # Do not pad the list with quantisation/FFT-leakage noise:
                # keep peaks within 80 dB of the strongest one and above an
                # absolute -100 dBFS floor.
                threshold = max(
                    strongest * 10.0 ** (-80.0 / 20.0),
                    10.0 ** (-100.0 / 20.0),
                )
                significant = local[amplitude[local] >= threshold]
                order = significant[np.argsort(amplitude[significant])[-5:][::-1]]
                peaks = [
                    {
                        "freq_hz": round(float(frequencies[index]), 2),
                        "amplitude_dbfs": round(to_db(float(amplitude[index])), 2),
                    }
                    for index in order
                ]

    return {
        "frames": frames,
        "duration": duration,
        "mono": mono,
        "peak_dbfs": round(to_db(peak), 2),
        "rms_dbfs": round(to_db(rms), 2),
        "silence_percent": round(silence_percent, 3),
        "windows": windows,
        "segment_start": segment_start,
        "segment_end": segment_end,
        "top_peaks": peaks,
    }


def choose_slices(windows, rate, frames):
    candidates = sorted(windows, key=lambda item: item["_rms"], reverse=True)[:12]
    candidates.sort(key=lambda item: item["_begin_sample"])
    for number, window in enumerate(candidates, 1):
        peak_sample = window["_peak_sample"]
        start = int(max(0, peak_sample - round(0.25 * rate)))
        if start + rate > frames:
            start = max(0, frames - rate)
        length = min(rate, frames - start)
        window["slice_start_sample"] = start
        window["slice_frames"] = length
        window["_slice_name"] = f"window-{number:03d}.wav"
    return candidates


def write_slice(path, payload, fmt, start_frame, frame_count):
    """Write a fresh 44-byte PCM (or 32-bit float) WAV header plus samples.

    The input may have zeroed RIFF/data sizes (qemu wav audiodev), so never
    copy its header; only the raw sample bytes are reused.
    """
    block = fmt["block_align"]
    begin = start_frame * block
    end = (start_frame + frame_count) * block
    data = payload[begin:end]
    fmt_chunk = struct.pack(
        "<HHIIHH",
        fmt["tag"],
        fmt["channels"],
        fmt["rate"],
        fmt["rate"] * block,
        block,
        fmt["bits"],
    )
    body = (b"fmt " + struct.pack("<I", len(fmt_chunk)) + fmt_chunk +
            b"data" + struct.pack("<I", len(data)) + data)
    riff = b"RIFF" + struct.pack("<I", len(body) + 4) + b"WAVE" + body
    with open(path, "wb") as handle:
        handle.write(riff)


# ---------------------------------------------------------------------------
# drawing


def draw_waveform(mono, rate, metrics, path, title):
    width, height = 1600, 460
    left, right, top, bottom = 92, 30, 62, 72
    plot_w = width - left - right
    plot_h = height - top - bottom
    duration = metrics["duration"]
    image = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(image)
    font = load_font(16)
    small = load_font(13)
    title_font = load_font(22, bold=True)

    draw.text((left, 18), title, font=title_font, fill=TEXT)
    draw.rectangle([left, top, left + plot_w, top + plot_h], fill=PANEL)

    for value, label in ((1.0, "+1.0"), (0.5, "+0.5"), (0.0, "0"),
                         (-0.5, "-0.5"), (-1.0, "-1.0")):
        y = top + (1.0 - value) * 0.5 * plot_h
        colour = AXIS if value == 0 else GRID
        draw.line([(left, y), (left + plot_w, y)], fill=colour)
        draw.text((left - 12, y), label, font=small, fill=MUTED, anchor="rm")

    step = nice_step(duration)
    tick = 0.0
    while tick <= duration + 1e-9:
        x = left + (tick / duration) * plot_w if duration else left
        draw.line([(x, top), (x, top + plot_h)], fill=GRID)
        draw.text((x, top + plot_h + 10), fmt_time(tick), font=small,
                  fill=MUTED, anchor="ma")
        tick += step
    draw.text((left + plot_w / 2, height - 24), "time",
              font=font, fill=MUTED, anchor="ma")
    draw_vertical_text(image, "amplitude", font, MUTED, 10,
                       top + plot_h / 2)

    if mono.size:
        edges = np.linspace(0, mono.size, plot_w + 1).astype(np.int64)
        centre = top + plot_h / 2
        for column in range(plot_w):
            begin = int(edges[column])
            end = int(max(edges[column + 1], begin + 1))
            segment = mono[begin:end]
            if not segment.size:
                continue
            low = max(-1.0, min(1.0, float(segment.min())))
            high = max(-1.0, min(1.0, float(segment.max())))
            y_low = centre - low * plot_h / 2
            y_high = centre - high * plot_h / 2
            if y_high > y_low:
                y_low, y_high = y_high, y_low
            draw.line([(left + column, y_high), (left + column, y_low + 1)],
                      fill=WAVE)
        draw.line([(left, centre), (left + plot_w, centre)], fill=AXIS)

    verdict = metrics["verdict"]
    verdict_colour = GOOD if verdict == "audio-present" else BAD
    summary = (f"peak {metrics['peak_dbfs']:.1f} dBFS   "
               f"rms {metrics['rms_dbfs']:.1f} dBFS   "
               f"silence {metrics['silence_percent']:.1f}%   "
               f"verdict ")
    draw.text((left, top + plot_h + 32), summary, font=font, fill=TEXT)
    summary_width = draw.textlength(summary, font=font)
    draw.text((left + summary_width, top + plot_h + 32), verdict,
              font=font, fill=verdict_colour)

    image.save(path)
    return path


def choose_n_fft(rate):
    n_fft = 1024
    while n_fft < 4096 and n_fft * 2 <= rate * 0.1:
        n_fft *= 2
    return n_fft


def draw_spectrogram(mono, rate, metrics, path, title):
    width, height = 1600, 640
    left, right, top, bottom = 98, 128, 66, 82
    plot_w = width - left - right
    plot_h = height - top - bottom
    duration = metrics["duration"]
    image = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(image)
    font = load_font(16)
    small = load_font(13)
    title_font = load_font(22, bold=True)
    draw.text((left, 18), title, font=title_font, fill=TEXT)

    n_fft = choose_n_fft(rate)
    hop = max(1, n_fft // 4)
    window_fn = np.hanning(n_fft).astype(np.float32)
    scale = 2.0 / float(window_fn.sum()) if window_fn.sum() else 1.0

    if mono.size:
        padded = mono.astype(np.float32, copy=False)
    else:
        padded = np.zeros(n_fft, dtype=np.float32)
    if padded.size < n_fft:
        padded = np.concatenate([padded, np.zeros(n_fft - padded.size,
                                                   dtype=np.float32)])
    total_frames = 1 + (padded.size - n_fft) // hop
    if total_frames < 1:
        total_frames = 1
    bins = n_fft // 2 + 1
    db_matrix = np.full((bins, total_frames), DB_FLOOR, dtype=np.float32)

    view = np.lib.stride_tricks.sliding_window_view(padded, n_fft)[::hop]
    batch = 256
    for start in range(0, total_frames, batch):
        stop = min(total_frames, start + batch)
        block = view[start:stop].astype(np.float32) * window_fn
        spectrum = np.abs(np.fft.rfft(block, axis=1)) * scale
        db_matrix[:, start:stop] = array_db(spectrum.T)

    column_edges = np.linspace(0, total_frames, plot_w + 1).astype(np.int64)
    rows = np.linspace(bins - 1, 0, plot_h)
    small_db = np.empty((plot_h, plot_w), dtype=np.float32)
    for column in range(plot_w):
        begin = int(column_edges[column])
        end = int(max(column_edges[column + 1], begin + 1))
        column_values = db_matrix[:, begin:end].max(axis=1)
        small_db[:, column] = np.interp(rows, np.arange(bins), column_values)

    normalised = np.clip((small_db - (-100.0)) / 100.0, 0.0, 1.0)
    rgb = LUT[(normalised * 255.0).astype(np.int32)]
    image.paste(Image.fromarray(rgb), (left, top))

    # Grid and labels over the spectrogram.
    step = nice_step(duration)
    tick = 0.0
    while tick <= duration + 1e-9:
        x = left + (tick / duration) * plot_w if duration else left
        draw.line([(x, top), (x, top + plot_h)], fill=GRID)
        draw.text((x, top + plot_h + 10), fmt_time(tick), font=small,
                  fill=MUTED, anchor="ma")
        tick += step
    nyquist = rate / 2.0
    for index in range(6):
        frequency = nyquist * index / 5.0
        y = top + plot_h * (1.0 - index / 5.0)
        draw.line([(left, y), (left + plot_w, y)], fill=GRID)
        draw.text((left - 12, y), fmt_hz(frequency), font=small,
                  fill=MUTED, anchor="rm")
    draw.text((left + plot_w / 2, height - 24), "time",
              font=font, fill=MUTED, anchor="ma")
    draw_vertical_text(image, "frequency (Hz)", font, MUTED, 10,
                       top + plot_h / 2)

    # Colour bar.
    bar_x = left + plot_w + 20
    for row in range(plot_h):
        level = 1.0 - row / max(plot_h - 1, 1)
        draw.line([(bar_x, top + row), (bar_x + 22, top + row)],
                  fill=tuple(int(v) for v in LUT[int(level * 255.0)]))
    draw.rectangle([bar_x, top, bar_x + 22, top + plot_h], outline=AXIS)
    for level in (0, -20, -40, -60, -80, -100):
        y = top + (0.0 - level) / 100.0 * plot_h
        draw.text((bar_x + 30, y), f"{level}", font=small, fill=MUTED,
                  anchor="lm")
    draw.text((bar_x + 30, top - 18), "dBFS", font=small, fill=MUTED,
              anchor="lm")

    subtitle = (f"STFT Hann n_fft={n_fft} hop={hop}; "
                f"0..{fmt_hz(nyquist)} Hz; colour -100..0 dBFS")
    draw.text((left, top - 20), subtitle, font=small, fill=MUTED)
    image.save(path)
    return path


def draw_overview(waveform_path, spectrogram_path, out_path, title, metrics):
    waveform = Image.open(waveform_path).convert("RGB")
    spectrogram = Image.open(spectrogram_path).convert("RGB")
    width = max(waveform.width, spectrogram.width)
    header = 96
    gap = 12
    height = header + waveform.height + gap + spectrogram.height + 16
    image = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(image)
    title_font = load_font(28, bold=True)
    font = load_font(16)
    draw.text((32, 18), title, font=title_font, fill=TEXT)
    verdict = metrics["verdict"]
    colour = GOOD if verdict == "audio-present" else BAD
    detail = (f"{metrics['duration']:.2f} s   {metrics['rate']} Hz   "
              f"{metrics['channels']} ch   {metrics['bits']}-bit   "
              f"peak {metrics['peak_dbfs']:.1f} dBFS   "
              f"rms {metrics['rms_dbfs']:.1f} dBFS   "
              f"silence {metrics['silence_percent']:.1f}%   verdict ")
    draw.text((32, 56), detail, font=font, fill=MUTED)
    detail_width = draw.textlength(detail, font=font)
    draw.text((32 + detail_width, 56), verdict, font=font, fill=colour)
    image.paste(waveform, (0, header))
    image.paste(spectrogram, (0, header + waveform.height + gap))
    image.save(out_path)
    return out_path


# ---------------------------------------------------------------------------
# main


def main():
    parser = argparse.ArgumentParser(
        description="Analyse a WAV capture and render visual evidence."
    )
    parser.add_argument("--wav", required=True, help="input PCM WAV")
    parser.add_argument("--out-dir", required=True, help="output directory")
    args = parser.parse_args()

    wav_path = os.path.abspath(args.wav)
    out_dir = os.path.abspath(args.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    if not os.path.isfile(wav_path):
        if os.path.basename(wav_path) == "capture.wav":
            # qemu's wav audiodev only creates the file when the guest first
            # opens the PCM voice.  Absence is a measurement, not an error.
            report = {
                "tool": "tools/audio-report.py",
                "version": 1,
                "wav": wav_path,
                "capture_exists": False,
                "verdict": "no-audio-file",
                "detail": ("capture.wav does not exist; qemu creates it only "
                           "when the guest opens the PCM voice, so the driver "
                           "produced no audio in this run"),
                "duration_s": 0,
                "rate_hz": None,
                "channels": None,
                "bits_per_sample": None,
                "frames": 0,
                "peak_dbfs": None,
                "rms_dbfs": None,
                "silence_percent": None,
                "non_silent_windows": [],
                "window_count": 0,
                "loudest_window": None,
                "top_fft_peaks": [],
                "slices": [],
                "images": {},
            }
            report_path = os.path.join(out_dir, "report.json")
            with open(report_path, "w", encoding="utf-8") as handle:
                json.dump(report, handle, indent=2)
                handle.write("\n")
            print(f"audio-report: {wav_path}")
            print("  verdict: no-audio-file")
            print(f"  {report['detail']}")
            print(f"  report: {report_path}")
            return 0
        sys.stderr.write(f"audio-report: input not found: {wav_path}\n")
        return 2

    try:
        fmt, payload, samples, riff_info = parse_wav(wav_path)
    except (WavError, OSError, struct.error) as error:
        sys.stderr.write(f"audio-report: cannot read {wav_path}: {error}\n")
        return 2

    analysis = analyse(samples, fmt["rate"], fmt["channels"])
    duration = analysis["duration"]
    windows = analysis["windows"]
    verdict = "audio-present" if windows else "silence"

    metrics = {
        "duration": duration,
        "rate": fmt["rate"],
        "channels": fmt["channels"],
        "bits": fmt["bits"],
        "peak_dbfs": analysis["peak_dbfs"],
        "rms_dbfs": analysis["rms_dbfs"],
        "silence_percent": analysis["silence_percent"],
        "verdict": verdict,
    }

    slices_dir = os.path.join(out_dir, "slices")
    os.makedirs(slices_dir, exist_ok=True)
    slices = choose_slices(windows, fmt["rate"], analysis["frames"])
    for window in slices:
        filename = window.pop("_slice_name")
        slice_path = os.path.join(slices_dir, filename)
        write_slice(
            slice_path,
            payload,
            fmt,
            int(window["slice_start_sample"]),
            int(window["slice_frames"]),
        )
        window["slice"] = os.path.relpath(slice_path, out_dir)

    for window in windows:
        for key in ("_begin_sample", "_end_sample", "_peak_sample", "_rms",
                    "slice_start_sample", "slice_frames"):
            window.pop(key, None)
        window.setdefault("slice", None)

    waveform_path = draw_waveform(
        analysis["mono"], fmt["rate"], metrics,
        os.path.join(out_dir, "waveform.png"),
        f"waveform - {os.path.basename(wav_path)}",
    )
    spectrogram_path = draw_spectrogram(
        analysis["mono"], fmt["rate"], metrics,
        os.path.join(out_dir, "spectrogram.png"),
        f"spectrogram - {os.path.basename(wav_path)}",
    )
    overview_path = draw_overview(
        waveform_path, spectrogram_path,
        os.path.join(out_dir, "overview.png"),
        f"audio report - {os.path.basename(wav_path)}", metrics,
    )

    report = {
        "tool": "tools/audio-report.py",
        "version": 1,
        "wav": wav_path,
        "format": "PCM" if fmt["tag"] == 1 else "IEEE float",
        "duration_s": round(duration, 6),
        "rate_hz": fmt["rate"],
        "channels": fmt["channels"],
        "bits_per_sample": fmt["bits"],
        "frames": analysis["frames"],
        "riff": riff_info,
        "peak_dbfs": analysis["peak_dbfs"],
        "rms_dbfs": analysis["rms_dbfs"],
        "silence_percent": analysis["silence_percent"],
        "silence_threshold_dbfs": SILENCE_DBFS,
        "db_floor": DB_FLOOR,
        "non_silent_windows": windows,
        "window_count": len(windows),
        "loudest_window": {
            "start_s": round(analysis["segment_start"] / fmt["rate"], 4)
            if fmt["rate"] else 0.0,
            "end_s": round(analysis["segment_end"] / fmt["rate"], 4)
            if fmt["rate"] else 0.0,
            "top_fft_peaks": analysis["top_peaks"],
        },
        "top_fft_peaks": analysis["top_peaks"],
        "slices": [window["slice"] for window in windows if window.get("slice")],
        "images": {
            "waveform": waveform_path,
            "spectrogram": spectrogram_path,
            "overview": overview_path,
        },
        "verdict": verdict,
    }

    report_path = os.path.join(out_dir, "report.json")
    with open(report_path, "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2)
        handle.write("\n")

    print(f"audio-report: {wav_path}")
    print(f"  verdict: {verdict}")
    print(f"  duration: {duration:.3f} s  rate: {fmt['rate']} Hz  "
          f"channels: {fmt['channels']}  {fmt['bits']}-bit")
    print(f"  peak: {analysis['peak_dbfs']:.2f} dBFS  "
          f"rms: {analysis['rms_dbfs']:.2f} dBFS  "
          f"silence: {analysis['silence_percent']:.2f}% "
          f"(threshold {SILENCE_DBFS:.0f} dBFS)")
    print(f"  non-silent windows: {len(windows)}  slices: {len(slices)}")
    if analysis["top_peaks"]:
        peaks = ", ".join(
            f"{peak['freq_hz']:g} Hz ({peak['amplitude_dbfs']:.1f} dBFS)"
            for peak in analysis["top_peaks"]
        )
        print(f"  top fft peaks: {peaks}")
    print(f"  images: {waveform_path}")
    print(f"          {spectrogram_path}")
    print(f"          {overview_path}")
    print(f"  report: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
