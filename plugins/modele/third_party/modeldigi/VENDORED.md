# ModelDigi: the vendored oscillator bank

The files in `dsp/` and `LICENSE` are copied unchanged from ModelDigi, a Model D-style oscillator section:

    https://github.com/simonjohansson/ModelDigi
    commit c412bdf2ab3b0b5b62484e8195c2f880d1964c67 (2026-10-23)
    licence GPL-2.0-or-later (LICENSE, each source file's SPDX line)

Taken: dsp/oscillator.c, dsp/oscillator.h, dsp/edge_kernel.h, dsp/mixer_curve.h, LICENSE. Nothing else of the
repository is here (no firmware glue, recordings, docs or tools). To update: copy these files from a newer commit,
note it here and in ../../NOTICE, run `make check`.
