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

## Stages (user, 2026-11-16) — overrides the order below
**Stage 1 — a decent working TTS for speech-dispatcher, soon.** What works now is enough;
no new engines beyond those listed here.
1. Piper as a resident module: the Piper voices are part of the synthesizer sherpa-onnx
   (2026-11-16; before, a second name piper-sherpa-onnx), model loaded once, two models kept
   (Piper ru + Supertonic en without reloading); replaces Pied / sd_generic
   piper-generic.conf, which start piper per message. Pauses between sentences as Piper's
   own (silence_scale 1, 0.2 s after each piece). Test, make it the default Piper path.
2. One or two more engines on the same runtime: Supertonic 3 (31 languages), Pocket, Kitten,
   Kokoro v1.0 — the sherpa-onnx models already packaged locally.
3. Maximum language coverage from these models: a table language → engine/voice (Piper
   voices per language, Supertonic, RHVoice multiverse voices, Kokoro), free licences only;
   gaps go to the list in RESEARCH.
4. Everything into ppa:gtubolcev/linux-tts (resolute + noble): engines, modules, voices
   within the quota, the rest through the downloader in this app.
5. Firefox through speech-dispatcher properly: (a) a patch proposal for Firefox's speechd
   backend (dom/media/webspeech/synth/speechd): honour the default module/voice/language,
   readable voice names instead of "Голос 1 (ru)"; (b) a Read Aloud fork for our case: voice
   per language from the speechd config, mixed ru/en text split into language runs.
6. **Acceptance tests, near the end of stage 1 (user, 2026-11-17):** the Firefox reader
   (speechd-reader) and the voices on the 100 most popular sites in the world and the 100 most
   popular in the Russian internet.
   - Lists:
     - Tranco top list (tranco-list.eu, research use, stable). Fix the date of the list.
     - Runet: a LiveInternet/Yandex rating, or Tranco limited to .ru/.рф plus the big
       Russian sites on .com (vk, yandex, mail, avito, ozon, wildberries, dzen, habr, 4pda…).
     - Leave out CDNs and API hosts that have no page (googleapis, akamai…), then fill up
       to 100.
   - For each site take the main page **and** one typical text page: article, post, product,
     topic. Main pages are often not readable. Keep the URL list in the repo so runs can be
     repeated.
   - Automated pass: Firefox on Xvfb with the "all sites" right, and the fake host recording
     the text. Per page record:
     - whether the script got in (CSP, sandboxed frames);
     - the panel: shown, not covering the page's own controls, not broken by the site's CSS;
     - the ▶ buttons: placed at paragraphs;
     - "Read": what Readability picked (the first and last sentences, the word count) and the
       language per block;
     - the picker on the main block;
     - errors in the console;
     - a screenshot.
   - What the user sees (user, 2026-11-17): besides saving the HTML and scripts, take a
     full-page screenshot and OCR it (tesseract rus+eng; Firefox's own screenshot of the
     whole page). The OCR text is the reference of what is on screen: compare it with what
     was read (fuzzy match per block) to find visible main text that was skipped and read
     text that isn't visible (hidden menus, off-screen or aria-hidden parts, cookie
     banners). Also the order: reading should follow the visual order of the blocks.
     Screenshots only after the cookie banner is handled (Consent-O-Matic in the test
     profile), else the banner covers the page.
   - Manual pass over the results table:
     - right text or not (menus, ads, comments read?);
     - highlight follows the reading;
     - mixed ru/en splits into the right voices;
     - then a listening spot check with real speechd voices on ~10 pages (to files only).
   - Speech check by several recognizers (user, 2026-11-17): the audio of the real voices
     (files only, never played) is transcribed by several STT systems and compared with the
     text that was sent:
     - for Russian: GigaAM (v2/v3 CTC/RNNT, Sber, MIT), Whisper (large-v3 / turbo), Vosk
       (the big ru model), T-one if it is ready;
     - for English: Whisper, Parakeet/Canary (NeMo), Vosk en.
     Output: WER/CER per sentence and per recognizer. A sentence is flagged when most
     recognizers disagree with the text. Disagreement between the recognizers themselves
     points to unclear speech, and the same error in all of them to a TTS or normalization
     error (numbers, Latin, stress). This uses the same tools as `linux-tts/scripts/norm-test`
     (`asr.py`, Vosk), extended to several engines, and runs in docker on mikhail-desktop.
   - Expected problems to note per site:
     - login walls, cookie banners, captchas;
     - geo-blocking: some Runet sites block foreign IPs, and some world sites block Russian
       ones;
     - SPAs that load text late;
     - infinite feeds;
     - shadow DOM, iframes.
   - Saved copies to work on (user, 2026-11-17): save a couple of pages per site once,
     then run the tests and the fixes on the copies, served by a local `http.server`. This
     is repeatable, and sites change or block over time. Keep the copies outside git, in
     `_tmp/sites/<date>/`; they are for private tests only.
     - `wget -p -k -E -H` keeps the files but not what JS draws after loading, and many big
       sites (SPAs, lazy text) render with JS.
     - So also save the rendered DOM from Firefox itself, with a SingleFile-style snapshot
       or `document.documentElement.outerHTML` after load and scroll.
     - Compare the two copies with the live site in the first runs.
   - Heavy news portals get extra pages: cnn.com, bbc.com, nytimes.com, ria.ru, lenta.ru,
     rbc.ru and the like. Their structure is complex: live blogs, galleries, embedded
     videos, "read more" blocks inside the article, lazy-loaded paragraphs, paywalls. Take
     3–5 pages of different kinds per portal (a plain article, a live blog, a page with video,
     a long read). Check that the article text is read whole and in order, and that nothing
     from teasers, captions or ads gets in.
   - Outcome: a table site → works / partly / no, with the reason. The fixes go into the
     extension (per-site corrections, `siteCorrections`), and a summary goes into the
     release notes.
**Stage 2 — the full solution, as experts would do it:** ggml / audio.cpp / CrispASR, GPU,
kokoro.cpp, text preparation (FST, stress), new heavy models, the GUI redesign items.

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
   - espeak-ng phonemizer (libespeak-ng → GenerationConfig.phoneme_codepoints;
     sherpa-onnx 2.x has no built-in one) shared by sd_sherpa and a new sd_piper:
     Kokoro v1.0 (en ~28 voices, es, fr, it, pt; Apache) and Kitten (en, 8) in
     sd_sherpa; Piper (40+ languages, hundreds of voices, licence per voice) as its
     own package speech-dispatcher-piper (the user, 2026-11-14: "Piper хотелось бы
     иметь отдельным пакетом, как все остальные"), on the sherpa-onnx runtime, not
     piper1-gpl. Replaces Pied and the sd_generic piper-generic.conf (a process and
     a model load per message, sox for rate/pitch, no volume, a downloaded binary).
     Voices: a few per language as packages within the PPA quota, the rest through
     a downloader in the settings app (Pied's other job). NC voices: downloader only.
     The built modules become GPL-3 through libespeak-ng (sources stay MIT).
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
   - **Light voices for uk / be, survey 2026-11-14** (user: "украинский и белорусский
     интересны"). Details in linux-tts/RESEARCH-tts-engines.md ("Ukrainian, Belarusian and
     other languages"). Steps: (1) Ukrainian — Piper uk_UA voices and Coqui uk-mai via
     sd_sherpa, no new code; (2) Belarusian — ask alex73 about the training data of
     BelarusianTTSviaCoquiTTS (Glow-TTS + HiFi-GAN, Apache-2.0), export to ONNX, small
     runner; fallback Coqui be glow-tts (CC-BY-SA); watch YauhenBichel/belarusian-tts for
     a Piper be voice. Not suitable: XTTS fine-tunes (CPML), OmniVoice (too slow on CPU),
     loudr-1 (no ru/uk/be, 2.6 GB).
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

## Tests to run before each release (user, 2026-11-17)
- `tests/gui-modules.sh`: click through every synthesizer and look at the screenshots. For each,
  check which voices are offered and whether the other controls appear as they should: the
  "Voices…" button, "Keep the synthesizer loaded", the voice type row.
  First run, 2026-11-17, ru, isolated speechd:
  - kokoro, sherpa-onnx, sherpa-onnx-vulkan-rs, silero, teratts and vosk all list their voices;
  - "Voices…" appears only for sherpa;
  - rhvoice was missing from the list because it had no audio server in the test (it fails
    its audio init without one). Still to check that it is offered in a real session.
  - Bug: after a synthesizer with the "Voices…" row (sherpa), the dialog keeps its height, and
    for every later synthesizer the "Close" button sits partly below the window edge. Either
    resize to the natural height on every switch, or keep a fixed row for the button.

## Voices and text options per language (user, 2026-11-20)
- A voice installer for every module, not only Piper: Supertonic models for
  sherpa-onnx(-vulkan-rs), Kokoro, Silero, Vosk, … — own voices of any module
  can be added from the dialog.
- In the lists every voice says which engine it is for ("Piper", "Supertonic",
  …); today the Piper list opens for all sherpa-onnx synthesizers with only a
  note line above it.
- Text normalization per language: one "Normalize the text" check box (numbers,
  dates, abbreviations) shown for every language that has a normalizer; the
  dialog asks the module which languages it covers instead of the hard-coded
  "ru".  Stress and ё stay a Russian-only box.  German: show the box once a
  German normalizer exists, until then none.

## Next

- **Explicit "other languages" synthesizer** (user, 2026-11-18: "DefaultModule где я могу
  поменять? В gui"). Today DefaultModule is set implicitly: the Language field is
  DefaultLanguage, and the synthesizer chosen while it shows that language goes to both
  LanguageDefaultModule and DefaultModule. Add a row "Other languages: [synthesizer]" bound
  to DefaultModule alone, and stop writing DefaultModule from the per-language combo.
  User's variant (preferred, 2026-11-18): a check box under the synthesizer, "Use for
  other languages too". Checked when the selected synthesizer is DefaultModule; checking
  sets DefaultModule. Unchecking has no meaning (some module must be the default), so the
  box can't be unchecked; its label names the current default ("now: teratts") so the user
  sees where to go.
- **Order (user, 2026-11-17):** 1) the Firefox extension, plus patches for Firefox itself
  proposed upstream (its speechd backend: speechd defaults, per-language voice);
  2) then the third voice source (sherpa models). Also: mirror all downloadable voices
  (Piper, RHVoice, sherpa models) on the user's GitHub — licences to check per voice,
  GitHub releases (2 GiB per file) rather than LFS (1 GiB free quota).
- **Bug (user, 2026-11-16, later):** the modal Piper Voices dialog can go behind the main window
  after a click outside it; the main window then takes no input. Check its transient parent
  (likely a GtkPlug in the settings manager, so the WM does not keep it on top).
- **Universal voice manager** (user, 2026-11-16: "сделать загрузчик (менеджер) голосов более
  универсальным, с выбором того, для какого движка поставить"): see the proposal in the chat of
  that day; the Piper part now downloads from the Piper collection itself (a39dac1).
- **Language first — done** (user, 2026-11-17: "уже сделано"). (user, 2026-11-11: "ставим первым в GUI (и логически) именно язык, и уже
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
    names, the "All languages" section, Voice type out.
  - Done 2026-11-17: the Synthesizer choice is written as `LanguageDefaultModule` for the
    selected language (and `DefaultModule` for the default language); a language whose module
    has no voice for it gets the first module that has one. The dialog applies its edits to
    speechd.conf as it is on disk when saving, so lines written meanwhile by hand stay.
- eSpeak / eSpeak NG hidden from the Synthesizer list and their languages from the Language
  list (user, 2026-11-11: "их синтез ужасен, нет смысла возиться").
- **RHVoice voice per language** (user, 2026-11-11): RHVoice's sd_module (rhvoice source package,
  not speechd) reads no configuration and picks the n-th male/female voice of the language by
  voice type, so 12 of 18 Russian voices are unreachable. Patch: `RHVoiceVoice "<lang>" "<voice>"`
  read from the module config (argv[1]), winning over the voice type; the GUI picks it up through
  the commented example in rhvoice.conf. Local deb first, upstream PR to RHVoice/RHVoice later
  (only if the user asks). Patch script: _tmp/rhv/apply-voice-patch.py.
- **Check that rate / pitch / volume really work** (user, 2026-11-13: "по моим ощущениям, эти
  опции сейчас чаще ни на что не влияют"). For every module (sd_zonos3, sd_sherpa, sd_vosk,
  piper, supertonic, pocket-generic, rhvoice, ...) and a voice or two each: spd-say with
  -r/-p/-i at the extremes, compare length (rate) and pitch/RMS of the output (no playback:
  write to a file or capture the audio stream). Table of what works; then either make our
  modules honour them (length scale, resampling, gain) or grey the sliders out for an
  engine that ignores them, with a tooltip.
- **Check punctuation and capital letters the same way** (user, 2026-11-14). Punctuation is
  only partly checked (the "said three times" bug, mode Some); None, Most and All not at all.
  Capital letters ("Say “capital”", "Play a sound") not checked at all. For every module:
  a test text with , ; : ! ? ( ) - " @ # and words with capitals (also all-caps and a
  single capital letter), each mode, output to a file and checked through STT or by length;
  "Play a sound" without playback (the icon goes to the audio server, so check the module
  log / speechd log instead of listening). Same table as above; a mode a module ignores is
  noted in the tooltip or handled in our modules.
- Check the 560 px default width on a real screen.
- **Only the main synthesizer in memory** (user, 2026-11-14: "нужно только один держать в
  памяти, только проставленный текущим основным в интерфейсе"). Found: speechd keeps every
  module it started alive with its model loaded — on the 5.7 GB laptop sd_vosk ~1 GB,
  sd_teratts ~0.8 GB, sd_kokoro ~0.4 GB, festival ~0.3 GB, all pushed into swap.
  Plan: (1) our modules load the model lazily on the first SPEAK, not at INIT, and free it
  after a few idle minutes (common/ code, a timer in the module's main loop; reload on the
  next SPEAK, first sound later by the load time). The main synthesizer chosen in the GUI:
  same by default; a check box "Load at login" (for heavy TTS users, user 2026-11-14:
  "можно эту загрузку сделать опциональной") preloads it at session start and keeps it
  loaded. The other modules never preload. (2) checked 2026-11-14: speechd starts all
  configured modules at once, and our modules load the model at INIT (after a speechd
  restart: sd_vosk 1047 MB, sd_teratts 814, sd_kokoro 442 right away; rhvoice 30, the rest
  3-11), so (1) is what helps; still check whether AddModule / the "Use All" config can
  limit which modules start; the GUI then lists only the chosen ones, other engines still selectable by
  clients like Orca. Measure RSS before and after.
- README.

## Later
- **Later list (user, 2026-11-16):**
  - check everything with Orca and other screen readers (speechd clients: Orca, Fenrir,
    speakup/espeakup, KDE's Qt TextToSpeech): voices, languages, module start, fallback;
  - a standalone speech-settings in Qt, and a KDE variant (System Settings module);
  - checks on Red Hat / CentOS Stream (and Fedora): packaging, speechd versions, paths;
  - variants for other distributions, chosen by distrowatch or, better, real usage data;
  - look again at *BSD support (speechd on FreeBSD/OpenBSD ports, audio, our modules).
- **Russian text preparation for all modules** (user, 2026-11-14). Supertonic reads
  "в 2026 году" as "в две тысячи двадцать шесть году", MMS drops digits and Latin words,
  Piper (espeak-ng) reads numbers without case and English words with English phonemes.
  Needed: numbers in the right case and gender (years, dates, quantities after
  prepositions), Latin words in Russian text (a dictionary of common words + English
  pronunciation, e.g. CMUdict), abbreviations, Roman numerals. Two ways: (a) rule FSTs
  for sherpa-onnx (`rule_fsts`, OpenFst format; maybe NeMo text processing's Russian
  grammars, Apache-2.0, compiled to that format; patch sherpa if needed) — only sd_sherpa;
  (b) our own library in linux-tts/common/ run before every engine. Order (user,
  2026-11-14): first (a), the FST rules for sherpa; once they exist, (b) as a separate
  library for the other modules (it can reuse the same FSTs). Before writing:
  1. search the Telegram group «Распознавание и синтез речи» (t.me/speech_recognition_ru)
     for existing solutions (normalisation, числительные, падежи, RUNorm, NeMo);
  2. check NeMo text processing ru, RUNorm (author of ruaccent; licence, size), num2words;
  3. a set of hard phrases to compare them; then choose.
  Rules should cover most cases; a small model only for the rest, not an LLM.
- **Heavy models: outside this project, maybe one trial module later** (user,
  2026-11-14). Voice LLMs and omni models (Qwen3-TTS, CosyVoice3, Higgs, VibeVoice,
  OmniVoice, Haru-4B…) need an NVIDIA RTX 4090-class card. Their users also need `sd_`
  modules to use them under Linux, but that is a separate project. Possible trial: one
  module for one large model that prepares the text itself — homographs, stress,
  emotions, voice cloning. Candidates and notes: linux-tts/RESEARCH-tts-engines.md,
  "Heavy". On mikhail-desktop (GTX 1060) only small or quantised models would fit.
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
  **Tested 2026-11-14** (onnx-vulkan-rs 8625ef9, ORT 1.29.0, docker; scripts in
  /home/grigory/Storage/Claude/onnx-vulkan, built on mikhail-desktop): ru_RU-dmitri-medium,
  all 2371 nodes on Vulkan, output equal to the CPU (atol 1e-2, noise off). Per phrase
  (195 phonemes): Vega 8 (RADV) CPU EP 245–440 ms wall / ~1.9 s CPU time (all threads),
  Vulkan 240–370 ms wall / ~0.05 s CPU time: same speed, CPU load ÷40. GTX 1060: CPU 138 ms,
  Vulkan 124 ms. Intel HD 620 (Kaby Lake GT2, ANV, 4-thread i5): CPU EP ~190 ms / ~0.36 s CPU
  time, Vulkan 450–545 ms / ~0.1 s CPU time: 2.7× slower but still RTF ~0.23 for 2.26 s of
  audio, CPU load ÷3.5; so on old Intel iGPUs the CPU path stays the default. `optimize`: no tunable nodes, no gain. en_US-ryan-high from Pied fails
  (an If node, "attribute then_branch of type Graph"); the authors used a re-export.
  To use it: sherpa-onnx must load the plugin EP (RegisterExecutionProviderLibrary +
  AppendExecutionProvider_V2), our own ORT ≥1.23 (Ubuntu's is 1.23.2, the project pins
  1.29), Rust in the build; the bundle is prepared for fixed phoneme counts, so a real
  module needs length buckets (pad + input_lengths) or per-length compilation. One author,
  started 2026-10, not packaged anywhere. Also has a standalone pure-Rust engine (no ORT).
- **ggml instead of ONNX for the GPU (decided 2026-11-14).** N. Shmyrev advised the user
  privately to drop ONNX and write everything "in the .cpp spirit", i.e. on ggml. User's
  decision:
  1. The ONNX modules stay as they are (sd_sherpa-onnx, sd_piper-sherpa-onnx, Kokoro,
     TeraTTS, Vosk): they work on the CPU and give hundreds of voices for free.
  2. The onnx-vulkan-rs path above is frozen: the sherpa-onnx-vulkan-rs /
     piper-sherpa-onnx-vulkan-rs names stay as built, no further work on the plugin
     (one author, own ORT 1.29, no Supertonic/Pocket). The GPU path goes through ggml.
     **Checked 2026-11-16** (`onnx-vulkan inspect`, supertonic-3 int8): text_encoder and
     vector_estimator fully supported; only PRelu (duration_predictor, vocoder) and an
     inference-mode BatchNormalization (vocoder) are missing — trivial ops, or rewrite the 3
     nodes in the graph. The real work stays fixed dims (length buckets; Supertonic has masks)
     and the sherpa-onnx plugin EP. Postponed by the user ("не срочно"); ggml/.cpp is stage 2.
     **UI once it works (user, 2026-11-17):** the synthesizer menu shows only the ONNX
     entry (sherpa-onnx, not the -vulkan-rs twin); beside it a check box "Offload to the
     GPU (Vulkan)" picks the Vulkan module. Like "Keep the synthesizer loaded", show the
     box only when the Vulkan variant is installed (and a Vulkan device exists).
  3. Pilot: one model on ggml, Kokoro through kokoro.cpp (simonfxr, GGUF; see RESEARCH).
     Measure CPU vs onnxruntime, Vulkan on the Vega 8, .121 (HD 620) and the GTX 1060;
     peak RSS, quantised (Q8/Q4) vs f16. Decide from the numbers whether to port
     Piper/VITS and others.
  4. Waiting for Shmyrev's reply on which ggml TTS projects he means; start with those.
  5. **audio.cpp (2026-11-15, user: "Да") — main candidate for the ggml path**, ahead of
     kokoro.cpp: github.com/0xShug0/audio.cpp, Apache-2.0, one C++/ggml engine with a
     stable C API (libaudiocpp.so, include/audiocpp.h: opaque handles, ABI version,
     sessions, streaming) and many TTS/ASR/VAD models. Steps: build from source on
     mikhail (CPU + Vulkan); benchmark kokoro and qwen3_tts (ru) to files only (RTF, peak
     RSS) on the CPU, the GTX 1060 (Vulkan) and .121 (HD 620); then maybe an
     sd_audiocpp module. Packaging: own package with its vendored ggml etc. in a private
     dir (/usr/lib/<triplet>/audiocpp/, RUNPATH), never clashing with the distro libggml0.
     First results (2026-11-15, RESEARCH "audio.cpp"): Piper voices convert to its
     piper_tts and run on Vulkan (GTX 1060) at RTF ~0.04 incl. load — the one real GPU
     win; on the CPU Piper stays on sherpa-onnx. qwen3_tts / cosyvoice3 form a heavy tier
     (RTF 1.2–1.9 on a 1060). Mali (phones/SBCs) is not our target; a fallback to ONNX /
     onnx-vulkan-rs for it can come later (user, 2026-11-15).
     Compare with CrispASR (MIT, one ggml binary, 69 TTS backends, C-ABI, built-in
     Russian G2P with an 813k stress dictionary) before choosing the engine for
     sd_audiocpp / sd_crispasr (user: "Да", 2026-11-15).
  Own library versions (user, 2026-11-15): "Мы можем собирать самую свежую версию, просто
  ставить её независимо" — this applies to onnxruntime too, which lifts the 2026-11-10
  "Ubuntu's libonnxruntime only" decision for future builds (private dir + RUNPATH, no
  Provides/Conflicts with distro packages).
  Why ggml: Ubuntu 26.04 ships it (libggml0 0.9.11, libggml0-backend-vulkan, HIP, BLAS), so
  no private runtime; its Vulkan backend is mature (llama.cpp, whisper.cpp; ZONOS3 on the
  Vega 8, see RESEARCH); quantisation for weak machines; small plain C/C++. Cost: every
  model needs its own port; few TTS ports exist (kokoro.cpp, TTS.cpp proof of concept).
  The CUDA variant below becomes a ggml backend package rather than an ORT build, if the
  pilot works out.
- **CUDA variant** (user, 2026-11-14: "наверное, нужен вариант для cuda?"): synthesizers
  `sherpa-onnx-cuda` / `piper-sherpa-onnx-cuda`, the same sd_sherpa-onnx with Provider
  "cuda" (sherpa-onnx supports it upstream). Unlike Vulkan, CUDA runs every graph, so
  Supertonic and Pocket would run on the card too. Needs our private ORT 1.29 built with
  `--use_cuda` (the CUDA EP is a separate libonnxruntime_providers_cuda.so, loaded through
  providers_shared) as a package `onnxruntime-ep-cuda`, depending on CUDA 12 and cuDNN
  (multiverse / NVIDIA repo, ~1–2 GB). First measure on mikhail-desktop's GTX 1060:
  Supertonic CPU vs CUDA; Piper there gains little (CPU 138 ms, Vulkan 124 ms). Local /
  GitHub first, the PPA only if multiverse dependencies are fine there. The GTX 1060
  (Pascal) is only served by older CUDA (CUDA 13 dropped Pascal), but is fine for the
  measurement (user: "не к спеху").
- **Old laptops: CPU and RAM** (user, 2026-11-14: "всё это обсуждение в список дальнейших
  оптимизаций"; freeing RAM matters on old laptops). Measure each on .121 (HD 620, 4 threads,
  7 GB) and the Vega laptop: peak RSS + CPU time + wall per phrase, before choosing defaults.
  - ORT thread spinning off (`session.intra_op.allow_spinning=0`): the Vega CPU EP used ~1.9 s
    CPU time for 0.25–0.44 s wall, i.e. mostly idle spinning. One setting, try first.
  - Fewer threads (1–2): less total work, the desktop stays responsive.
  - Module priority: `nice`/`SCHED_IDLE` for the synthesis thread.
  - ORT memory: `enable_cpu_mem_arena=0`, `enable_mem_pattern=0` (lower peak RSS, buffers
    not kept between phrases).
  - mmap of the weights where the engine allows it (pages reclaimable, not swapped).
  - int8 models: weights ¼ of fp32. Costs: some quality (hiss/buzz, mostly in the VITS/HiFi-GAN
    decoder; Kokoro int8 slightly duller), and on CPUs without VNNI (Kaby Lake, Zen 1) quantized
    convolutions can be *slower* than fp32; only MatMul-heavy parts gain. Listen + measure.
  - Light models as the default on weak machines (Piper low/medium, Kitten TTS nano).
  - A disk cache of frequent phrases (menu items, notifications) in the modules.
  - XNNPACK EP (in sherpa-onnx) vs the default CPU EP.
  - Idle unload (done, item 2: 5 minutes, only the main synthesizer may stay).
  - GPU: on integrated graphics Vulkan only relieves the CPU, RAM stays the same (shared memory,
    plus driver overhead); Vega 8 ÷40 CPU, same speed; HD 620 2.7× slower, ÷3.5 CPU. So CPU
    stays the default on old Intel, Vulkan is an option (check box "Use the graphics card",
    `<Module>Gpu`, 0.1.0-13; today only sd_zonos3 has it).
  - Other GPU routes to check later: ORT **WebGPU EP** (official, Vulkan via Dawn on Linux,
    Intel contributes); **OpenVINO EP** (sherpa-onnx has `provider=openvino`, documented for
    KWS/VAD on NPUs, needs an ORT built with OpenVINO; Gen9 iGPUs need the frozen
    intel-compute-runtime legacy1 branch, so little gain on Kaby Lake; worth it from Xe and on NPUs);
    onnx-vulkan-rs (see the GPU item) for an experimental `SherpaGpu` in sd_sherpa/sd_piper.
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
  **User, 2026-11-16, after the redesigned Read Aloud:** it still downloads voices into its
  own cache, does not know the page language (and offers no choice), reads with whatever
  voice is selected, and picks the wrong text: it reads the page through in a row. Needed:
  voices from speechd (no own downloads), the page/paragraph language (lang attributes,
  else detection, plus a manual choice), and choosing what to read (selection, the main
  article, "read from here" on a block) instead of the whole page.
- **Universal voice manager — agreed** (user, 2026-11-16, "да"): the button becomes
  "Voices…", shown for every chosen synthesizer that has voices to download, and the
  window lists that engine's voices. First after Piper: RHVoice, sherpa models
  (Supertonic, MMS). Order: the manager first, then the Firefox extension.
  Later (user, postponed): language and engine filters in the manager window, to find
  one's way among all engines and languages. For now one engine: the chosen synthesizer.
  **Done 2026-11-17** (92fd721): generic window with Piper and RHVoice sources (RHVoice:
  distro package through PackageKit, else the rhvoice.org catalogue into
  ~/.local/share/RHVoice, needs rhvoice +voice3). Next source: sherpa models.
- **Firefox extension, notes** (user, 2026-11-16): study Read Aloud's internals first
  (how it finds the text, site handlers, highlighting), something may be reusable.
  **Element picker** (user, 2026-11-17), an ALTERNATIVE to the default, not the main way: by
  default the main text is found with Readability (Mozilla's library, as Firefox's Reader
  View does); the picker is for pages where Readability takes the wrong part or for reading
  just one block. Choose the part of the page like uBlock Origin's
  element picker or the DevTools inspector (hover highlights a block with a frame and its
  size/selector label, click picks it). The DevTools picker is not open to extensions, so
  an own one in the content script: overlay + elementFromPoint, wheel / arrow keys to go to
  the parent or child block, click = read this block, Shift+click = read from here to the
  end, Esc cancels; also fully by keyboard. Optional: remember the picked selector per
  site ("always read this region here", like uBlock's cosmetic filters). The block's
  `lang` (or detection) picks the voice.
  Readability is fine to start with, but many sites have no reader view, and on some it
  picks menus or side content as the main text: a manual choice of the region (pick a
  block, read from here, selection) must always be there.
- **Better language detection** (user, 2026-11-17, later): Firefox's
  browser.i18n.detectLanguage is CLD2 (character n-grams, no dictionary); it fails on
  short or mixed text (opennet.ru headlines: "ru 57%, sr 18%, unreliable"). Stopgaps now:
  detection over all the text to read, the last detected language, a Cyrillic letter hint.
  Candidates to compare on short headlines, mixed pages and close languages (ru/uk/be/bg/sr,
  de/nl, es/pt): lingua (Apache-2.0; rust with a wasm build, also python: best on short
  text, big models), fastText lid.176 (model CC-BY-SA, ~1 MB .ftz, fast), CLD3 (Apache-2.0,
  small NN). Where: in the content script (wasm) or in the native host (python, the models
  as a package in the PPA). Limit the choice to languages with an installed voice.
- **"Report a broken page"** (user, 2026-11-17, later; like Consent-O-Matic's "tell the
  authors about a page that doesn't work"): a button in the popup and the panel. It shows
  the user what goes out before sending: the URL, the extension version, which reading
  mode was used, the detected languages per block and the start of each unit read (to see
  what Readability took or skipped), an optional comment. No page text beyond that and no
  cookies without the user's consent. Simplest channel without a server: open a prefilled
  GitHub issue in the speechd-reader repo (URL length limits: long reports as a file to
  attach). The reports feed the site fixes and the acceptance tests (item 6).
