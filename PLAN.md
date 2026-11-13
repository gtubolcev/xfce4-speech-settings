# xfce4-speech-settings — plan

## Done (v0.1)
- Settings dialog / settings-manager plug for the speech-dispatcher defaults
  (module, language, voice type, rate/pitch/volume, punctuation, capitals, audio output, test).
- Edits `~/.config/speech-dispatcher/speechd.conf` in place, keeps comments, reloads the daemon (SIGHUP).

## Done (2026-11-10)
- "Use All" info bar for configs with `AddModule` lines: comments them out, copies a generic
  module config to `modules/NAME-generic.conf`, renames DefaultModule, reloads the server and
  refills the synthesizer list after 1.5 s. Tested on a copy of the user's config.
- The scales mark 0 as "Normal" (Volume 0 is the synthesizer's normal volume, not silence).
- Default width 560 px.
- (2026-11-11) The test sentence follows the language (built-in, untranslated sentences for
  16 languages); a text typed by the user is kept.
- (2026-11-11) "Voice" list: the real voices of the synthesizer for the language (user: "просто
  показывать доступные голоса"). The choice goes into the module config
  (`speechd_module_set_voice`): generic modules — the voice's AddVoice line gets the
  DefaultVoiceType type and moves behind the language's other lines (speechd: the last line for
  a language+type wins; a missing type falls back MALE1, FEMALE1, MALE2, …); sd_zonos3 ≥ 0.1-5 —
  `Zonos3Voice "<lang>" "<voice>"`. Native modules (espeak-ng, rhvoice): list greyed out, the
  "Voice type" row appears instead. The test speaks with the chosen voice.

## Roadmap (user, 2026-11-12, evening)
Three goals, in this order:
1. **Tidy the code** of the modules and the GUI so that a human programmer can read it:
   - move the build recipes that only live in the git-ignored `XFCE-patches/_tmp/sherpa`
     (sherpa-onnx 2.0.1-3, model packages supertonic-3, pocket, mms-ru, kitten-nano-en)
     into linux-tts; the others are in /home/grigory/Storage/Claude (speechd-src,
     rhvoice, piper-build, vosk, kokoro-ru) — move their recipes too;
   - `common/` in linux-tts: the code all five modules repeat (config parsing, unquote,
     SSML → text, sentence split, voice by name / pick_voice, send_audio,
     process_input, the module_* stubs) and the server code silero and zonos3 share
     (state file, spawn/adopt/watch/stop, Reader, json_escape) — about 400 lines;
   - one meson build instead of the docker build.sh scripts; `debian/` dirs instead of
     build-deb.sh (Launchpad needs source packages);
   - comments: what each module does, its threads, how STOP works;
   - the GUI (speech-page.c 1670 lines, speechd-config.c 1136): split, comment;
   - run every proto.py before and after, compare the WAVs.
2. **More voices**, en first, then the main European languages, then the former USSR.
   Now: Supertonic 3 (31 languages × the same 10 voices; has en, most EU, uk, lt, lv, et);
   missing everywhere: be, kk, ka, hy, az, uz, ky, tg; no, is, ca, sr, mk, sq, ga, cy, eu.
   - espeak-ng phonemizer in sd_sherpa → Piper (40+ languages, hundreds of voices,
     licence per voice), Kokoro v1.0 (en ~28 voices, es, fr, it, pt; Apache), Kitten
     (en, 8). No separate sd_piper: the model stays loaded, and piper1-gpl is GPL-3.
     The built sd_sherpa becomes GPL-3 through libespeak-ng (sources stay MIT).
   - RHVoice voices from multiverse (uk, ky, tt, uz, pl, cs, sk, mk, sq, eo, pt-br):
     check that the GUI shows them.
   - Silero v3 / v4_cyrillic (kk, ky, uz, tt, ba, udm, xal, …; en 118 speakers, de,
     es, fr, ua): NC → installer only. MMS (be, hy, az, tg, tk, …): NC, one voice each.
   - Kitten: more English timbres by mixing its 8 style vectors (256 × 400 rows per
     voice), or a KVoiceWalk-like search against a reference recording. Kokoro vectors
     cannot be moved into Kitten (another model's style space).
   - sd_vosk, from the alphacep/vosk-tts issues (read 2026-11-12): vosk-tts stays Russian
     only (en/fr/tr requests answered "use Piper"). Worth doing: vosk-model-tts-ru-0.10-multi
     (834 MB; nshmyrev recommends it over 0.9 for pauses, #51) — probably multistream_v3,
     our port knows only v1: add v2/v3 (word positions, `phone_duration_extra` from upstream
     050118b "Duration extra for pauses", lowercase BERT). "+" before a vowel marks stress
     (#4; our g2p handles it) → a user stress dictionary in the GUI. Latin names in Russian
     text (#47) — share a transliteration with Silero. Unknown symbols (#50, KeyError
     upstream) do not break sd_vosk (tested). The slow 0.8+ load (#46) is the
     FindReusableTensor issue we avoid with ORT_PARALLEL — the user may post that in #46.
   - Tips from nshmyrev (2026-11-13): **TeraTTSv2** (TeraSpace, HF TeraSpace/TeraTTSv2) — a
     Supertonic derivative, ru + en, 10 styles (ru_f1, ru_f2, ru_m1, ru_m5, 6 English), 44.1 kHz,
     ONNX only (text_encoder, duration_predictor, one 8-step sampler graph, vocoder; .npy
     styles; unicode_indexer.json). The sampler graph differs from Supertonic's
     vector_estimator, so sherpa's Supertonic code does not run it: a small C runner of
     ~300 lines (like kokoro.c), `<ru>`/`<en>` tags, "+" stress marks — from RUAccent
     (MIT, bundled; dictionary mode = 21 MB, no neural nets) or our vosk stress placer.
     Licence OpenRAIL-M + attribution only for commercial products → same check as
     Supertonic. **Shtorm-PocketTTS-RU** (ArtShtorm, CC-BY-4.0 → PPA ok): Pocket TTS
     fine-tuned on Russian (fast/slow variants, audiobook style, SentencePiece 4000);
     needs acute stress marks (U+0301) → vosk stress placer; voice from a 5 s reference
     (consent!) — references could be our own synthetic voices. Only safetensors: export
     to ONNX with sherpa-onnx's Pocket export script (in docker), then sd_sherpa's Pocket
     family runs it with a Russian tokenizer.
   - **Russian stress library in C** (user, 2026-11-13: "ruaccent и ещё какие-то ударяторы
     портировать на C"): `common/ru-stress`, a port of RUAccent (MIT, code and models,
     HF ruaccent/accentuator; TeraTTS has a compact 300-line Python adaptation to follow).
     Stages: (1) dictionary (accents.json, 21 MB gz → sorted mmap file like vosk's) + ё
     words + "+" from the user wins + a user dictionary from the GUI; (2) the char model
     for unknown words (0.8 MB ONNX, char vocab); (3) the tiny BERTs "needs stress?" and
     ё-homographs (WordPiece, reuse vosk.c's tokenizer); (4) optional: the homograph
     DeBERTa (360 MB, BPE tokenizer) — not for weak machines. Output both "+" and U+0301.
     Users: Shtorm and TeraTTS (they need it), Kokoro-ru (ru-g2p), unknown words of vosk,
     maybe Supertonic ru (test whether it takes stress marks). After common/.
     **First (user, 2026-11-13: "для начала пойдёт и от воск, так быстрее"):** take the
     stress placer out of sd_vosk into common/ (its dictionary + BERT homographs) and use
     it for Shtorm/TeraTTS; the RUAccent port comes later.
   - **Done 2026-11-13: sd_teratts** (linux-tts/teratts/: teratts.c engine, sd_teratts.c,
     teratts-test, build.sh, build-deb.sh; common/ru-stress.c = the vosk dictionary stress
     without espeak, "+" or U+0301; common/num-words.c = num2words ru/en). The C port
     matches the Python reference (same noise) to 7e-5. Text prep: Latin runs → `<en>`,
     Cyrillic → `<ru>`, "21 000", "3.14." and "1.2.3" handled better than teratts.py,
     dash → comma, & → и/and. ~3× faster than real time on 4 threads, load 1.9 s.
     Debs in ~/Storage/Claude/teratts-deb (speech-dispatcher-teratts 0.1-1~local,
     teratts-model-v2 2.0-1~local, both LICENSE files next to the model). GUI: "teratts"
     added to own_modules (not rebuilt yet). Done too: kokoro's ru-g2p.c uses
     common/ru-stress.c (IPA of 1539 lines identical; "+" before a vowel now sets
     the stress instead of being read "плюс"; speech-dispatcher-kokoro 0.1-3~local). TeraTTS speed: the sampler
     (8 Euler steps in one Loop graph) is ~75 % of the time and memory bound; int8
     (dynamic, MatMul or MatMul+Conv, Loop body extracted) is not faster or is slower
     and hurts the sound, 8 threads slower than 4; left: shorter first chunk, synthesize
     the next sentence while one plays. U+0301 after a vowel = "+" (sd_teratts 0.1-2).
     GUI 0.1.0-7: the test text is a GtkTextView that grows to 8 lines, Ctrl+Enter
     speaks, tooltip on "+". Ordinals ("5-й") and number agreement are not
     handled; streaming vocoder (16-frame chunks) if first-sound latency matters.
3. **Packages / PPA**: PPA ppa:gtubolcev/xfce-patches has only xfce4-settings and
   thunar-disk-usage-plugin (v0.4.1); a linux-tts PPA does not exist yet. Into it: our
   modules, sherpa-onnx, kitten (Apache), vosk-ru (Apache), Supertonic and Pocket after
   a licence check; never Silero or MMS (NC). Installed locally only (~local): speechd
   +fix1, rhvoice +voice1, sherpa-onnx, piper-tts, all modules and models;
   xfce4-speech-settings 0.1.0-5 built, 0.1.0-4 installed.

## Next
- **Language first** (user, 2026-11-11: "ставим первым в GUI (и логически) именно язык, и уже
  к нему привязываем конфигурацию"). The dialog starts with the language; everything that
  speechd can keep per language is set for that language:
  ```
  Language:     [Русский (ru)        ▾]  [x] Default language (texts without a language)
  ── Русский ───────────────────────────
  Synthesizer:  [ZONOS3              ▾]   only engines with a voice for this language
  Voice:        [ru-female-1         ▾]   the engine's own voices for this language
  ── All languages ─────────────────────
  Rate / Pitch / Volume, Punctuation, Capital letters, Output, Test
  ```
  - Language list: the union of the languages of all modules' voices, shown with the
    language name ("Русский (ru)"), configured languages first.
  - Synthesizer → `LanguageDefaultModule "<lang>" "<module>"`; for the default language
    also `DefaultModule` (texts without a language and clients that don't send one).
  - Voice: speechd has no per-language voice, so it goes into the module config:
    generic modules (piper/supertonic/pocket-generic): the chosen voice becomes the first
    `AddVoice "<lang>" "<DefaultVoiceType>" …` line of that language; our modules
    (sd_zonos3, sd_sherpa, sd_vosk): `<Module>Voice "<lang>" "<voice>"`; native modules
    without such a setting (espeak-ng, rhvoice): voice type only, marked as such.
  - Rate, pitch, volume, punctuation, capitals, output are global in speechd: an
    "All languages" section.
  - Voice type (DefaultVoiceType) leaves the main view (it is what the per-language voice
    replaces); clients that ask for a type still get the module's mapping.
  - Done 2026-11-11 (step 1): Language is the first row; its list is the union of all
    modules' languages (codes only); the Synthesizer list shows only the modules with a voice
    for it (the configured one stays, marked "no voice for this language"). Still open: language
    names, LanguageDefaultModule per language, the "All languages" section, Voice type out.
- eSpeak / eSpeak NG hidden from the Synthesizer list and their languages from the Language
  list (user, 2026-11-11: "их синтез ужасен, нет смысла возиться").
- **RHVoice voice per language** (user, 2026-11-11): RHVoice's sd_module (rhvoice source package,
  not speechd) reads no configuration and picks the n-th male/female voice of the language by
  voice type, so 12 of 18 Russian voices are unreachable. Patch: `RHVoiceVoice "<lang>" "<voice>"`
  read from the module config (argv[1]), winning over the voice type; the GUI picks it up through
  the commented example in rhvoice.conf. Local deb first, upstream PR to RHVoice/RHVoice later
  (only if the user asks). Patch script: _tmp/rhv/apply-voice-patch.py.
- Check the 560 px default width on a real screen.
- README.

## Later
- **Capital letters: later, not a main feature** (user, 2026-11-12). Options: a hint in the
  window; a test through SPD_SPELL; "заглавная" before single capital letters in sd_sherpa
  and sd_vosk (they ignore CAP_LET_RECOGN now).
- **"Show legacy engines" check box: maybe later** (user, 2026-11-12) — eSpeak, Festival,
  Flite, Pico are hidden (`hidden_modules` in speech-page.c); the box would show them.
  Also offer baratinoo, ivona, kali, voxin there (user removed the packages 2026-11-12; they
  need proprietary engines but may matter to blind users).
- **"Install" button for inactive engines**: show the engines that are known but not usable
  (module package not installed, e.g. speech-dispatcher-rhvoice / -festival / -pico;
  or the module is installed but has no voices, like RHVoice on Ubuntu) greyed out with an "Install" button.
  Packages through PackageKit (D-Bus / libpackagekit-glib2, no sudo in the app);
  voices (Piper, RHVoice, sherpa models) are downloaded to `~/.local/share/…`, then the module config
  is written and the daemon reloaded.
- New engines: native `sd_sherpa` module, sherpa-onnx package in a PPA (see RESEARCH-tts-engines.md).
- **A tab per installed engine** (user's proposal, 2026-11-10): the main tab keeps the
  speech-dispatcher defaults; each engine gets its own tab with its own properties.
  Piper tab ≈ what Pied does: list of voices by language with quality (x_low…high),
  download / remove a voice, play a sample, choose the voice per language, speed, then
  write the module config and reload the daemon. RHVoice tab: voices from rhvoice.su and
  rhvoice.conf settings; espeak-ng: variants, mbrola voices; later sd_sherpa models.
  The voice catalog below lives inside these tabs.
  **The Piper config must have one voice per language** (same format as Pied PR #57,
  github.com/Elleo/pied/pull/57; verified: `_tmp/speech-build/cfg/.../piper-generic.conf`):
  `GenericLanguage` + `AddVoice` per language with codes `en`, `ru` (`en-us`, `en-gb` and
  `en` → default when a language has several regions), `DefaultVoice`, command picks
  model/speaker from `$VOICE` (`model` or `model:speaker`), piper `--output_file -` (WAV).
  Why: speechd matches AddVoice languages exactly (lowercased); Firefox lists one voice
  per AddVoice line, picks it by name, and Narrate hides voices whose `lang.split("-")[0]`
  ≠ page language, so `en_US` (Pied ≤ 0.3.2) is never offered. dotconf expands `${…}`
  as env variables — no `${V%…}` in the command. Firefox sees only the voices of the
  DefaultModule (bugzilla 1821875).
- **Nuance Vocalizer voices (via Voxin, voxin.oralux.net): later, not urgent** (user,
  2026-11-12) — check whether Russian voices (Milena, Yuri?) are sold, price, quality vs
  RHVoice/vosk; speech-dispatcher-voxin is the module.
- **speechd audio leaks and does not recover: fixed locally 2026-11-12** in
  speech-dispatcher 0.12.1-2ubuntu1+fix1~local (debian/patches/reload-leaks.patch in
  /home/grigory/Storage/Claude/speechd-src/pkg; also the punctuation repeated once per
  SIGHUP). No write timeout added (death of the server wakes the writer). Not sent upstream.
  Original note: **speechd audio leaks and does not recover: later** (user, 2026-11-11: "запиши на потом
  разобраться с этим"). After a suspend the BT output went silent; the running speechd held
  21 PipeWire streams and had stopped logging after a SIGHUP. Two bugs in
  speech-dispatcher 0.12.1, also in brailcom/speechd master:
  1. the server opens a stream per module (`output_open_audio`, src/server/output.c) but
     never calls `spd_audio_close`: `reload_output_module` / `unload_output_module` /
     `destroy_module` (src/server/module.c) leave it open — every SIGHUP (our window sends
     one, speechd-config.c) leaks the stream of each module that has spoken;
  2. src/audio/pulse.c: on a write error `pulse_feed` closes the connection but keeps
     `pa_current_rate/bps/channels`, so `pulse_begin` never reopens it and the next write
     gets a NULL `pa_simple`; `spd_pa_simple_write` also waits for writable space without
     a timeout (a suspended sink can block it for good);
  3. punctuation said three times (user, 2026-11-11: "Привет, запятая, запятая, запятая"):
     `symbols_preprocessing_add_file` (src/server/symbols.c) appends to `symbols_files` and
     a SIGHUP reload reads speechd.conf again without clearing it, so each HUP adds the six
     .dic files once more; the processor of a locale is built on its first message from
     the whole list, so a language first spoken after two HUPs runs symbols.dic three
     times. Reproduced on a private silent instance (_tmp/spdtest). Fix: clear the list
     and the cached processors (G_symbols_dicts and the per-locale processors) on reload.
  Fix: local deb 0.12.1-2ubuntu1+fix1~local — close the module stream on unload/reload
  (stop the speak queue first if it is the speaking one), reset the parameters on a write
  error, a timeout on the wait. Source: /home/grigory/Storage/Claude/speechd-src. Report
  upstream only when the user asks. Until then: restart speechd instead of HUP when stuck.
- **Vosk TTS as a C port** (user, 2026-11-11, replaces "revive vosk-tts-rs"): the models are
  plain ONNX (see RESEARCH, "Vosk TTS through plain ONNX Runtime"). Port the Python
  vosk_tts pipeline (alphacep, Apache-2.0: dictionary, g2p.py, WordPiece for BERT,
  multistream ids) to C on the system libonnxruntime C API; the Python code is the
  reference, andreytkachenko/vosk-tts-rs (Apache-2.0, abandoned 2026-04, multistream v1–v3,
  BERT) a second one where useful. Why not the Rust port itself: no C API (lib + gRPC
  server), downloads ONNX Runtime, cargo deps in the PPA build, ~33 MB binary. Result: a
  resident speechd module (sd_vosk, the model loads once, like sd_zonos3), packages for the
  linux-tts PPA (engine/module + model). Check model versions first (0.9-multi, 0.10).
- **Quality tiers in the engine list** (user, 2026-11-11, after listening): festival, flite,
  espeak, espeak-ng, pico sound unacceptable today; Piper and RHVoice are fine only as a
  "robotic" voice; the expected quality is the modern neural one (ZONOS3 voices, Supertonic,
  Pocket). Decision: **keep Piper and RHVoice as normal engines; festival, flite, espeak,
  espeak-ng, pico go into a "Legacy" group**, hidden by default; a check box
  "Show legacy synthesizers" (somewhere in the dialog) shows them for whoever still needs them.
  A configured DefaultModule from the legacy group stays visible even when they are hidden.
  The main effort goes to modern engines through speechd (sd_sherpa, zonos3 server module).
- **Engines that can't speak the selected language** (user, 2026-11-11): with
  DefaultLanguage `ru` and DefaultModule `pocket-generic` (English only) the user heard
  gibberish that sounded like Chinese. The synthesizer list must hide engines without a voice for the
  selected language, or show them greyed out with "(no <language> voice)". The language list should
  likewise mark the languages the chosen engine doesn't support. Source of truth: the
  module's voice list (`spd_list_synthesis_voices` languages). Changing the language
  re-filters the list. A configured DefaultModule stays visible, with a warning.
- **GPU for ONNX engines: postponed** (user, 2026-11-11). Ubuntu's libonnxruntime 1.23.2 is
  CPU only (+ oneDNN). Candidate: github.com/automataIA/onnx-vulkan-rs (Vulkan plugin EP,
  Rust, Apache-2.0, Piper among the tested models). Open questions: not validated on AMD,
  pins ORT 1.29, needs fixed input dims, sherpa-onnx/piper must register the plugin EP.
  Quick test first: build in docker, `onnx-vulkan run` on ru_RU-dmitri-medium on the Vega 8.
  Decide after the ZONOS3 ggml-Vulkan result. (Result 2026-11-11: ZONOS3 on the Vega 8 via
  ggml Vulkan is RTF 0.55 vs 0.96 on the CPU, CPU load ÷12, see RESEARCH, so GPU is worth it.)
- **Wide engine survey** (user, 2026-11-11), after sd_vosk and the remaining candidates of this
  list: every TTS/STT system mentioned in the Telegram community "Распознавание и синтез речи"
  (Shmyrev's group), one table: licence, languages (ru/en), runtime (ONNX / ggml / PyTorch),
  CPU speed, Vulkan path, packaging effort.
- **Stress placers ("ударяторы") survey: later** (user, 2026-11-11) — compare the text
  pre-processors that put Russian stress marks; see the next item.
- **Russian stress marks** (user, 2026-11-11, after listening to Supertonic): "кафе" comes out
  as "к+афе" instead of "каф+е". Later: pre-process the text with a Russian stress
  placer (candidates to check: RUAccent, silero-stress; licences and whether they run offline
  without Python) and pass the marks in the form each engine understands (Piper/ZONOS/Supertonic
  differ: `+`, U+0301, or none). Could be a shared filter in the modules (sd_sherpa, sd_zonos3).
  **Decision (user, 2026-11-12):** first reuse the stress placer of sd_vosk (dictionary +
  BERT) for xVibePocketTTS / Kokoro-ru; later find all stress placers, port them to C and
  let the user choose one (shared library used by every module).
- **sd_kokoro 0.1-1 built** (2026-11-12): the Russian Kokoro (zaakirio/kokoro-ru) in C —
  ru-g2p.c (stress from the Vosk dictionary, ru_g2p.py + misaki + phonemizer ported, output
  identical to the Python reference on 16 test lines), espeak-ng with ru_dict from master
  (honours U+0301), fp32 ONNX (0.7 s per s of speech on 4 threads; int8 was 2 s on ORT 1.23).
  Voices sveta, masha, dima (dima is a separate model: switching reloads, ~3.5 s). User
  (2026-11-12): "качество синтеза очень высокое, интонации хорошие, ошибок в ударениях я не
  заметил", a bit "художественные" (audiobook style). Packages: speech-dispatcher-kokoro,
  kokoro-tts-model-ru (weights OpenRAIL — check before the linux-tts PPA). Later: share the
  stress dictionary as its own package instead of depending on vosk-tts-model-ru-0.9-multi;
  BERT homograph choice as in sd_vosk.
- **GPU backends per vendor** (user, 2026-11-11). ZONOS3 (ggml): no ONNX variant needed —
  ggml has CUDA, HIP/ROCm, SYCL (Intel), Vulkan, Metal; with `GGML_BACKEND_DL` the backends
  are loadable libs, so: one `zonos3-tts` + backend packages (Vulkan = the universal
  default; CUDA/ROCm/SYCL for those cards). Ties in with "own ggml package"; check whether
  ZONOS3 runs on plain ggml or needs Zyphra's servers/ggml/patches. ONNX engines
  (Supertonic, Pocket, Piper, Vosk): ONNX Runtime execution providers per vendor (CUDA/
  TensorRT, MIGraphX, OpenVINO), CPU for the rest (Vulkan EP maybe later); Ubuntu's ORT is
  CPU only, GPU builds would be ours.
  **To research (user, 2026-11-11): for every engine, look for non-ONNX runtimes**
  (ggml/GGUF ports, llama.cpp-style C++, Vulkan/compute-shader implementations),
  because ONNX Runtime has no usable Vulkan path: Supertonic, Pocket (Kyutai has a
  Rust/candle implementation?), Piper, Vosk/VITS, Kokoro, Kitten, Matcha.
- **ZONOS3 dropped as a system voice** (user, 2026-11-11, after listening to the trimmed pairs):
  ZONOS3-Mini adds noise before or after the speech in about a quarter of the phrases, for every
  voice and seed; trimming the audio at pauses did not remove it ("в остальных фразах много
  галлюцинаций"). Next candidates: Supertonic (ru/en), Piper, RHVoice, Vosk TTS (C port);
  look for Vulkan-capable runtimes.
  Order (user, 2026-11-11): **Supertonic is the main voice** (sd_sherpa); sd_vosk may start
  now; RHVoice is acceptable for Russian too; Vulkan experiments only after the other options
  are exhausted — then kokoro.cpp (ggml) + the Russian Kokoro (zaakirio/kokoro-ru) first.
  sd_sherpa 0.1-1 built (2026-11-11, see RESEARCH "sd_sherpa"); next (user, 2026-11-11:
  "можно воск тогда передвинуть вперед"): sd_vosk first, then Kitten/Kokoro/Piper through
  libespeak-ng phonemes.
- **sd_zonos3 ignores the voice type** (user, 2026-11-11: "при выборе разных голосов ничего не
  меняется, голос один, мужской"). The dialog sets DefaultVoiceType (MALE1…FEMALE3, CHILD_*),
  the module only honours `synthesis_voice` by name (module_set ignores "voice"). Also all
  four Russian embeddings are male (median F0: seed1 127 Hz, seed7 108, seed11 129, seed23
  128; en female ≈ 176 Hz), so there is no Russian female voice at all. Fix: (1) generate
  female Russian embeddings (more seeds, keep F0 > 165 Hz, or encode Russian text spoken
  with the en-*-female embedding, ru-brf.wav was 179 Hz), let the user pick by ear;
  (2) name voices `<lang>-<male|female>-<n>` in zonos3-model-mini; (3) module_set "voice":
  MALEn/FEMALEn → the n-th voice of that gender for the language (fallback: first of the
  gender, then any); an explicit synthesis_voice still wins.
- **The "Voice type" list does not follow the engine** (user, 2026-11-11: "при смене движка в
  настройках список голосов вообще не изменился"). It is speechd's fixed DefaultVoiceType list;
  each module maps the types to its voices (or ignores them, like sd_zonos3 now). Plan:
  modules report the gender in SPDVoice.variant ("male"/"female"); the dialog shows only
  the types the engine can serve (or marks the empty ones) and, next to the combo, the
  voice that will actually speak; choosing a concrete engine voice belongs in the engine tab
  (written to the module config: DefaultVoice / type → voice mapping).
- Voice catalog: filter by language, sample playback.
- **Models / voices under special licences** (user, 2026-11-12, first case: Silero v5_ru,
  CC BY-NC 4.0). Launchpad PPAs accept only free (DFSG/OSI-style) licences, so NC or
  otherwise restricted files never go into a .deb; the package ships only a manifest and
  the downloader (the Debian contrib "installer" pattern, cf. ttf-mscorefonts-installer).
  Manifest entry per file: id, engine, languages, URL, sha256, size, licence (SPDX id + URL
  of the full text + one-line summary, e.g. "non-commercial use only"), upstream homepage.
  In the engine tab such entries show an "Install for non-commercial use…" button: a dialog
  with the licence summary and full text, the user must accept; then download, check
  sha256, save to `~/.local/share/<app>/voices/<engine>/<id>/` together with LICENSE,
  SOURCE (URL, sha256, date, accepted licence) and README from upstream; remove deletes the
  whole dir. Free models go the same way (or as .debs) but without the acceptance step.
  Models that are executable code (torch.package / pickle, e.g. Silero) are loaded only
  from the pinned sha256 in the manifest.
- **A voice for weak machines: KittenTTS 0.8** (user, 2026-11-12: "Не у всех nvidia 4090").
  github.com/KittenML/KittenTTS; nano 15M (int8 25 MB, fp32 56 MB), micro 40M, mini 80M;
  ONNX, StyleTTS 2 like Kokoro, espeak-ng phonemes, English only, 8 voices, 24 kHz;
  weights Apache-2.0 → fit the linux-tts PPA. A module = sd_kokoro with another model
  (or through sherpa-onnx, which has a Kitten family). Measure on the Ryzen 5 3500U: time
  to first audio and real-time factor at 1–2 threads, compare with Kokoro and Piper.
  Maybe a "low resources" hint in the engine tab picks it. Not KittenTTS 2 (1.7B, ru among
  20 languages): Stellon Labs Community License (revocable, AUP not public, "Powered by
  KittenML", no training on its output) and too slow to start for a screen reader; at most
  a later "read a long text" mode, model downloaded by the user.
- Speech input (STT) tab. User's idea (2026-11-11): rewrite the UI of an existing
  dictation app in GTK rather than design one from scratch: github.com/cjpais/Handy
  (Rust/Tauri, MIT, ~38k stars) or one of the dozen similar apps. Engine candidate:
  github.com/handy-computer/transcribe.cpp (C/C++ on ggml, MIT, Vulkan; Whisper, Parakeet,
  Moonshine, GigaAM-v3 for Russian, streaming). It is STT only, nothing for synthesis.
  On the Vega 8 check the iGPU device type first (the ZONOS3 trap: IGPU ≠ GPU in ggml).
- GNOME port as a prototype (the backend `speechd-config.c` is toolkit-independent).
- **Firefox: later** (user, 2026-11-12). Firefox just lists every voice it gets from
  speechd and ignores the default module, voice, rate, pitch and volume set in this dialog
  ("криво это сделано"). Bilingual texts work badly: a Russian voice skips the English
  words. Plan: rewrite the Read Aloud extension or write our own — pick the voice per
  language (default from speechd.conf / LanguageDefaultModule), split mixed text into
  language runs and speak each run with the matching voice. Check Firefox's speechd
  backend (dom/media/webspeech/synth/speechd) for what it overrides.
