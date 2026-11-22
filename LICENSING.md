# Licensing

This file records where every part of xfce4-speech-settings comes from, under which licence
it is used, and the rules for dependencies and for the voices the dialog downloads.
State of 2026-11-22 (licence audit). Open problems are listed at the end.

## The repository licence

xfce4-speech-settings is GPL-2.0-or-later (`COPYING`), like Xfce itself. All commits are by
Grigory Tuboltsev (with Claude as co-author); GitHub lists no other contributors and no pull
requests. The first commit (3bc7a24) came from gtubolcev/xfce-patches
(`speech-settings/`), which had the same single author.

Own files carry

    Copyright (C) 2026 Grigory Tuboltsev
    SPDX-License-Identifier: GPL-2.0-or-later

`src/*.c` and `src/*.h` have the GPL-2+ notice but no SPDX line yet.

## What the GPL-2.0-or-later repository can take

| Licence of the piece | Copy or port into the repository | Link against |
|---|---|---|
| MIT / Expat, BSD-2/3-Clause, ISC, Zlib | yes, keep the notice | yes |
| LGPL-2.1-or-later, GPL-2.0-or-later | yes | yes |
| **Apache-2.0** | **no** (incompatible with GPL-2) | **no** |
| **GPL-3.0-only, GPL-3.0-or-later, LGPL-3** | **no** (would make the program GPL-3) | **no** |
| MPL-2.0 | only as separate MPL files | yes |

So code from linux-tts (GPL-3.0-or-later), sherpa-onnx (Apache-2.0), misaki (Apache-2.0),
piper1-gpl (GPL-3) or espeak-ng (GPL-3+) must not be copied or linked here. The dialog
talks to them only as separate programs and data (speech-dispatcher modules, files on disk),
which is not linking. Reading their documentation or scripts to reimplement a file format
(for example the metadata sherpa-onnx's `add_meta_data.py` appends to Piper models) is
fine; copying their code is not.

## Distribution rule: PPA, downloader, nowhere

The same rule as in linux-tts `LICENSING.md`:

1. **PPA:** only free licences (DFSG / OSI style), including the training data of models.
2. **Downloader (this dialog):** voices and models the user may use privately although they
   are not free (NC, ND, OpenRAIL-M, research-only training data). Before the download the
   dialog must show the licence summary and full text and the user must accept it; the
   licence and a SOURCE note (URL, sha256, date, accepted licence) are saved next to the
   voice. Free voices are downloaded without the acceptance step but still with their
   licence file.
3. **Nowhere:** voices without a licence, of unknown origin, or whose licence forbids
   redistribution or private use by others.

The downloader never fetches executable code from the net; models that are executable
(torch.package, pickle) may only be loaded from a sha256 pinned in the package.

## Inventory

| Path | Origin | Licence | Holders | Relation |
|---|---|---|---|---|
| `src/`, `tests/`, `meson.build`, `build-deb.sh`, `scripts/` | own | GPL-2.0-or-later | Grigory Tuboltsev | own |
| `src/voice-unzip.c` | own minimal ZIP reader | GPL-2.0-or-later | Grigory Tuboltsev | own |
| `src/voice-source-piper.c` metadata writer | format of sherpa-onnx `scripts/piper/add_meta_data.py` (Apache-2.0) | GPL-2.0-or-later | Grigory Tuboltsev | consulted (reimplemented) |
| `data/icons/xfce4-speech-settings.svg` | own drawing (commit aa9eb28) | GPL-2.0-or-later | Grigory Tuboltsev | own |
| `data/*.desktop.in` | own | GPL-2.0-or-later | Grigory Tuboltsev | own |
| `po/*.po`, `po/*.pot` | own translations | GPL-2.0-or-later (same as the package) | Grigory Tuboltsev | own |
| `debian/` | own | GPL-2.0-or-later | Grigory Tuboltsev | own |

Libraries linked: GTK 3, GLib/GIO (LGPL-2.1+), libxfce4ui, libxfce4util (LGPL-2+),
json-glib (LGPL-2.1+), libsoup 3 (LGPL-2+), libspeechd (LGPL-2.1+), PackageKit-glib
(LGPL-2.1+). All are compatible with GPL-2.0-or-later.

## What the dialog downloads

| Source | URL | Licence information used | Status |
|---|---|---|---|
| Piper voices | `https://huggingface.co/rhasspy/piper-voices` (voices.json, MODEL_CARD) | the `License:` line of the model card, shown before the install | free and non-free voices are mixed; the card names the dataset licence, not the base model's; fine-tuned voices inherit the base model's restrictions (lessac: research only) |
| RHVoice voices | `https://rhvoice.org/download/packages-1.14.json`, README of each GitHub voice repo | the licence named in the README, shown before the install | many voices are CC-BY-NC-ND-4.0 or custom |

Downloaded voices are data for separate programs; their licence does not affect the licence
of the dialog, but the user must be told the terms (see the distribution rule).

## Names

"Xfce" in the name follows the naming of Xfce components; the project is not part of Xfce
unless Xfce adopts it. Engine and voice names (Piper, RHVoice, Kokoro, Vosk, Silero) are used
only to describe what the dialog works with.

## Checklist: adding a dependency or a voice source

1. Record the origin (URL, version or commit) and read the licence file itself.
2. Code: allowed only under the licences in the table above (no Apache-2.0, no GPL-3).
3. Libraries: dynamic linking against LGPL is fine; check the whole chain of what is linked.
4. Voice source: find where the licence of each voice is stated and whether it covers the
   base model and the training data. Map each licence to PPA / downloader / nowhere and make
   the dialog ask for acceptance of every non-free licence.
5. Save the licence text and the SOURCE note next to every installed voice.
6. Add a row to the inventory above and a stanza to debian/copyright if files are copied.

## Open problems (2026-11-22)

1. The dialog shows a voice's licence but does not ask the user to accept non-free
   licences, and it does not warn that a Piper voice fine-tuned from lessac is research-only
   whatever its card says.
2. The model card or licence text is not saved next to an installed voice (Piper: the
   MODEL_CARD is read only for its `License:` line).
3. The translation headers still carry template placeholders ("THE PACKAGE'S COPYRIGHT
   HOLDER", "Automatically generated").
4. No SPDX lines in most files.
