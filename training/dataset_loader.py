"""
V9.5 — Loader du dataset mastering utilisateur.

Charge les paires (raw, mastered) depuis /home/michael/data/dataset_mastering/.
Corrige les symlinks cassés au runtime (path /home/michael/dataset → data).
"""

import json
import os
from pathlib import Path
from typing import Iterator, NamedTuple

import numpy as np
import soundfile as sf


DATASET_ROOT = Path('/home/michael/data/dataset_mastering')
MANIFEST = DATASET_ROOT / 'manifest.json'


class Pair(NamedTuple):
    slug: str               # track_slug from manifest
    genre: str              # suggested_master_genre
    raw_path: Path          # resolved real raw path
    master_path: Path       # mastered output
    source_dataset: str     # cambridge / musdb / dsd
    style: str


def fix_path(p: str) -> str:
    """Corrige les paths cassés du manifest (dataset → data)."""
    return p.replace('/home/michael/dataset/', '/home/michael/data/')


def load_manifest():
    with open(MANIFEST) as f:
        return json.load(f)


def iter_pairs(filter_genres=None, require_exists=True) -> Iterator[Pair]:
    """Yield Pair pour chaque entrée du manifest avec input + output existants."""
    for entry in load_manifest():
        # raw input
        raw_src = entry.get('input_mix', {}).get('source', '')
        if not raw_src:
            continue
        raw_path = Path(fix_path(raw_src))

        # master output
        master_rel = entry.get('expected_master_output', {}).get('full_path', '')
        if not master_rel:
            continue
        master_path = DATASET_ROOT / master_rel

        # genre filter
        genre = entry.get('suggested_master_genre', '') or 'Unknown'
        if filter_genres and genre not in filter_genres:
            continue

        # existence check
        if require_exists:
            if not raw_path.exists() or not master_path.exists():
                continue

        yield Pair(
            slug=entry.get('track_slug', ''),
            genre=genre,
            raw_path=raw_path,
            master_path=master_path,
            source_dataset=entry.get('source_dataset', ''),
            style=entry.get('style', '') or '',
        )


def load_audio(path: Path, target_sr: int = 48000):
    """Charge un wav (mono ou stéréo) au format float32 stéréo @ target_sr.
    Mono → dupliqué L/R. Resample si nécessaire via librosa (ou skip si OK)."""
    audio, sr = sf.read(str(path), dtype='float32', always_2d=True)
    # Mono → stéréo dup
    if audio.shape[1] == 1:
        audio = np.repeat(audio, 2, axis=1)
    # Resample
    if sr != target_sr:
        import librosa
        # librosa expects (channels, samples)
        audio = librosa.resample(audio.T, orig_sr=sr, target_sr=target_sr).T
        sr = target_sr
    return audio.astype(np.float32), sr


def audio_stats(audio: np.ndarray) -> dict:
    """RMS, peak, crest factor (dB)."""
    rms = float(np.sqrt(np.mean(audio**2)))
    peak = float(np.abs(audio).max())
    crest = peak / (rms + 1e-9)
    return {
        'rms_lin': rms,
        'peak_lin': peak,
        'crest': crest,
        'rms_db': 20 * np.log10(rms + 1e-12),
        'peak_db': 20 * np.log10(peak + 1e-12),
    }


def quick_report():
    """Petit récap : combien de paires utilisables, par genre."""
    counts = {}
    n_total = 0
    for pair in iter_pairs():
        n_total += 1
        counts[pair.genre] = counts.get(pair.genre, 0) + 1
    print(f"Pairs utilisables : {n_total}")
    for g, n in sorted(counts.items(), key=lambda x: -x[1]):
        print(f"  {g:12s} : {n}")


if __name__ == '__main__':
    quick_report()
