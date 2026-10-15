"""The voice tools' clip editing, script reading and checking (docs/features/08 voice-library.md),
and the tracked voice: the ID registry, RVOC format 1, export, verify and pack (AUD-023, AUD-026;
voice-first-build.md D07, D08, D13). No model, encoder or build folder is needed."""
import array
import hashlib
import importlib.util
import json
import math
import re
import shutil
import struct
import tempfile
import unittest
import wave
import zlib
from pathlib import Path

VOICE = Path(__file__).resolve().parents[1] / "voice"


def load(name):
    spec = importlib.util.spec_from_file_location(f"voice_{name}", VOICE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ae = load("audio_edit")
carrier = load("carrier")
record = load("record")
bench = load("bench")
asr_check = load("asr_check")
LIB = json.loads((VOICE / "library.json").read_text(encoding="utf-8"))

RATE = 16000
# Revision 6's recording: each word cut from a carrier sentence, and an ending for every word
# (AUD-031). Revision 7 went back to the words spoken alone (decided 2026-10-10: clearer, and
# the forms took too much room), but the tools that made them (carrier.py, forms.py) stay.
FORMS_LIB = dict(LIB, carriers={
    "position": "{} door.", "location": "Back {} door.", "object": "Back {} two.",
    "number": "Zone {} door.", "word.zone": "Zone two door.",
    "number.100": "Zone one hundred door.",
    "word.deck.end": "Not ready. Check the zones. Upper deck.",
    "number.100.end": "Not ready. Check the zones. Zone one hundred.",
    "status.arming": "Arming away. You may exit now.", "status.armed": "Armed home.",
    "status.away": "Armed away.", "status.home": "Armed home.", "status.night": "Armed night.",
    "walk.walk_test": "Walk test started.", "walk.started": "Walk test started.",
    "walk.ended": "Walk test ended."},
    endings="Not ready. Check the zones. Back deck {}.", form_takes=8,
    carrier_cut={"quiet_db": 35, "near_db": 6, "reach_ms": 250, "slack_ms": 20, "tail_ms": 10,
                 "seconds": [0.1, 2.5]})


def tone(ms, amp, hz=440):
    return [amp * math.sin(2 * math.pi * hz * i / RATE) for i in range(RATE * ms // 1000)]


def rms(x):
    return math.sqrt(sum(s * s for s in x) / len(x))


class AudioEditTests(unittest.TestCase):
    def test_trim_cuts_silence_but_keeps_lead_and_tail(self):
        x = [0.0] * 8000 + tone(300, 0.5) + [0.0] * 8000
        y = ae.trim(x, RATE, lead_ms=30, tail_ms=80)
        # the 300 ms word plus 30 ms before and 80 ms after (to within one 10 ms frame)
        self.assertAlmostEqual(len(y) / RATE, 0.41, delta=0.011)
        self.assertEqual(y[0], 0.0)    # faded in
        self.assertEqual(y[-1], 0.0)   # faded out

    def test_trim_keeps_a_quiet_consonant_above_the_floor(self):
        word = tone(200, 0.5) + tone(100, 0.005, hz=4000)  # a quiet "s" 40 dB down
        y = ae.trim([0.0] * 4000 + word + [0.0] * 4000, RATE, floor_db=-45.0, tail_ms=0)
        self.assertGreaterEqual(len(y), len(word) - RATE // 100)

    def test_trim_of_silence_is_empty(self):
        self.assertEqual(ae.trim([0.0] * 1600, RATE), [])

    def test_level_reaches_the_target_without_passing_the_peak(self):
        y = ae.level(tone(500, 0.05), RATE, target_db=-20.0, peak_db=-3.0)
        self.assertAlmostEqual(20 * math.log10(rms(y)), -20.0, delta=0.1)
        loud = ae.level(tone(500, 0.05), RATE, target_db=0.0, peak_db=-3.0)
        self.assertLessEqual(max(abs(s) for s in loud), 10 ** (-3 / 20) + 1e-9)

    def test_join_puts_the_gap_only_between_clips(self):
        a, b, c = tone(100, 0.3), tone(200, 0.3), tone(50, 0.3)
        y = ae.join([a, b, c], RATE, gap_ms=60)
        self.assertEqual(len(y), len(a) + len(b) + len(c) + 2 * RATE * 60 // 1000)
        self.assertEqual(y[:len(a)], a)

    def test_wav_round_trip(self):
        x = tone(100, 0.5)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "t.wav"
            ae.write_wav(p, x, RATE)
            y, rate = ae.read_wav(p)
        self.assertEqual(rate, RATE)
        self.assertEqual(len(y), len(x))
        self.assertLess(max(abs(a - b) for a, b in zip(x, y)), 1 / 16000)


class SpeakerStageTests(unittest.TestCase):
    """AUD-015: the packs carry clear speech (speaker.py): each master's quiet ends trimmed and
    raised to -15 dB of voiced RMS with the gain capped at the peak, nothing limited (revision 7;
    the panel makes it louder, AUD-033); digital silence stays silent."""

    def test_the_pack_holds_clear_speech_at_minus_15(self):
        # Decided 2026-10-10: the loudness lives on the panel; the pack holds clear speech.
        speaker = load("speaker")
        s = speaker.SETTINGS
        self.assertEqual((s["target_rms_db"], s["limit"], s["lead_ms"], s["tail_ms"]),
                         (-15.0, False, 10, 20))

    def test_a_master_comes_out_louder_by_one_gain_under_the_peak_cap(self):
        speaker = load("speaker")
        x = ae.level([0.0] * 800 + tone(500, 0.3, 500) + [0.0] * 800, RATE)  # as a master is
        y = speaker.for_speaker(x, RATE, dict(speaker.SETTINGS, lead_ms=None))  # untrimmed
        self.assertEqual(len(y), len(x))
        cap = 10.0 ** (speaker.SETTINGS["peak_db"] / 20.0)
        self.assertLessEqual(max(abs(v) for v in y), cap + 1e-12)
        gain = y[1000] / x[1000]
        self.assertGreater(20 * math.log10(gain), 3.0)
        self.assertTrue(all(abs(b - a * gain) < 1e-12 for a, b in zip(x, y)))  # nothing but a gain
        self.assertEqual(speaker.for_speaker(x, RATE, dict(speaker.SETTINGS, lead_ms=None)), y)

    def test_the_quiet_ends_are_trimmed_to_the_settings(self):
        # 50 ms of silence each side; 10 ms kept before the first loud 10 ms frame, 20 after the
        # last (audio_edit.trim): the panel's word gap makes the pause between words.
        speaker = load("speaker")
        x = [0.0] * 800 + tone(500, 0.3, 500) + [0.0] * 800
        y = speaker.trimmed(x, RATE)
        self.assertEqual(len(y), 8000 + RATE * 10 // 1000 + RATE * 20 // 1000)
        self.assertEqual(len(speaker.for_speaker(x, RATE)), len(y))
        self.assertEqual(speaker.trimmed(x, RATE, dict(speaker.SETTINGS, lead_ms=None)), x)

    def test_sharp_peaks_hold_the_gain_back(self):
        speaker = load("speaker")
        x = tone(300, 0.05, 500)
        x[100] = 0.7  # one sharp peak
        y = speaker.for_speaker(x, RATE, dict(speaker.SETTINGS, limit=False))
        self.assertAlmostEqual(max(abs(v) for v in y), 10.0 ** (speaker.SETTINGS["peak_db"] / 20.0))

    def test_a_limited_level_limits_a_sharp_peak_and_keeps_the_voice_at_the_level(self):
        # Revision 6's "Louder -6, Limited" (chosen on the audio bench, 2026-10-09); the
        # bench's --limit still makes it.
        speaker = load("speaker")
        r6 = dict(speaker.SETTINGS, target_rms_db=-6.0, limit=True, lead_ms=None)
        x = [0.0] * 800 + tone(300, 0.05, 500) + [0.0] * 800
        x[1000] = 0.7  # one sharp peak, 23 dB over the tone
        y = speaker.for_speaker(x, RATE, r6)
        self.assertEqual(len(y), len(x))
        cap = 10.0 ** (speaker.SETTINGS["peak_db"] / 20.0)
        self.assertLessEqual(max(abs(v) for v in y), cap + 1e-12)
        self.assertGreater(speaker.voiced_db(y, RATE), -6.3)   # the voice reaches the level
        self.assertEqual(y[:800], [0.0] * 800)                 # digital silence stays silent
        before, after = y[810] / x[810], y[5590] / x[5590]   # 12 ms before it, 287 ms after
        self.assertAlmostEqual(after / before, 1.0, delta=0.015)  # the dip is only around it
        self.assertLess(y[1000] / x[1000], before / 5)
        self.assertEqual(speaker.for_speaker(x, RATE, r6), y)

    def test_a_spoken_word_ends_at_or_under_the_level_under_the_peak(self):
        # A word with sharp peaks stops short of -15 dB: its gain is capped at the peak
        # (README "The speaker stage"); its quiet ends are trimmed, never its sound.
        speaker = load("speaker")
        master = VOICE.parents[1] / "assets/voice/en-US/qwen3-designed_female/masters/word.front.wav"
        x, rate = ae.read_wav(master)
        y = speaker.for_speaker(x, rate)
        self.assertLess(len(y), len(x))
        self.assertGreater(len(y), len(x) - rate * 110 // 1000)  # at most 20 + 60 ms, frames
        cap = 10.0 ** (speaker.SETTINGS["peak_db"] / 20.0)
        self.assertLessEqual(max(abs(v) for v in y), cap + 1e-12)
        self.assertTrue(-18.0 <= speaker.voiced_db(y, rate) <= -14.9, speaker.voiced_db(y, rate))

    def test_the_pack_rate_keeps_the_ceiling(self):
        # The resampler to 16 kHz overshoots the stage's ceiling; the encoder holds it again.
        speaker, encode = load("speaker"), load("encode")
        cap = round(10.0 ** (speaker.SETTINGS["peak_db"] / 20.0) * 32767)
        pcm = [0] * 50 + [int(9000 * math.sin(i / 3.0)) for i in range(400)] + [31000, -30500]
        y = encode.ceiling(pcm)
        self.assertEqual(len(y), len(pcm))
        self.assertLessEqual(max(abs(v) for v in y), cap)
        quiet = [100, -2000, 9000, 0, -29000]
        self.assertEqual(encode.ceiling(quiet), quiet)  # under the ceiling: as it was

    def test_silence_stays_silent(self):
        speaker = load("speaker")
        self.assertEqual(speaker.for_speaker([], RATE), [])
        self.assertEqual(speaker.for_speaker([0.0] * 800, RATE), [0.0] * 800)


class LibraryTests(unittest.TestCase):
    def test_each_word_is_spoken_alone_and_each_sentence_as_written(self):
        # Revision 7: the words spoken alone ("original"), no endings (decided 2026-10-10).
        rows = {r["clip_id"]: r for r in record.clips_wanted(LIB)}
        self.assertEqual(rows["word.front"]["spoken"], "Front.")
        self.assertEqual(rows["word.patio"]["spoken"], "Pattio.")
        self.assertNotIn("word.door.end", rows)
        self.assertEqual(rows["status.exit_now"]["spoken"], "You may exit now.")
        self.assertEqual(rows["status.disarmed"]["spoken"], "Disarmed.")
        self.assertEqual(rows["extra.living"]["file"], "extra/living.wav")

    def test_each_word_is_spoken_in_its_carrier_with_the_forms_settings(self):
        rows = {r["clip_id"]: r for r in record.clips_wanted(FORMS_LIB)}
        self.assertEqual(rows["word.front"]["spoken"], "Front door.")
        self.assertEqual(rows["word.patio"]["spoken"], "Back pattio door.")
        # AUD-031: a word's ending is cut from the end of an announcement, after "Back deck".
        self.assertEqual(rows["word.patio.end"]["spoken"],
                         "Not ready. Check the zones. Back deck pattio.")
        self.assertEqual(rows["word.door.end"]["file"], "word.door.end.wav")
        self.assertEqual(rows["status.armed"]["spoken"], "Armed home.")

    def test_every_name_in_the_examples_is_a_clip(self):
        rows = [r for r in record.clips_wanted(LIB) if r["kind"] != "ending"]
        texts = {r["text"].lower() for r in rows}
        self.assertEqual(len(texts), len(rows), "two clips share a text")
        names = [w for e in LIB["examples"] for w in e]
        names += [w for whole, parts in LIB["compare"] for w in [whole] + parts]
        self.assertEqual([w for w in names if w != "|" and w.lower() not in texts], [])

    def test_every_word_has_an_ending_with_its_text_and_its_derived_id(self):
        # AUD-031: the ending of word N is 0xC000 + N, category "ending", the word's own text.
        rows = record.clips_wanted(FORMS_LIB)
        words = {r["clip_id"]: r for r in rows if r["kind"] == "token"}
        ends = [r for r in rows if r["kind"] == "ending"]
        self.assertEqual(sorted(r["base"] for r in ends), sorted(words))
        for r in ends:
            w = words[r["base"]]
            self.assertEqual(r["clip_id"], w["clip_id"] + ".end")
            self.assertEqual((r["category"], r["text"]), ("ending", w["text"]))
            self.assertEqual(int(r["numeric_id"], 16), 0xC000 + int(w["numeric_id"], 16))
        self.assertEqual(record.endings(list(words.values()), dict(FORMS_LIB, endings=None)), [])
        self.assertEqual(record.endings(list(words.values()), LIB), [])  # revision 7 has none

    def test_every_pinned_or_respelled_clip_is_in_the_script(self):
        ids = {r["clip_id"] for r in record.clips_wanted(LIB)}
        self.assertEqual([c for c in [*LIB.get("seeds", {}), *LIB["say"]] if c not in ids], [])

    def test_a_bar_is_the_pause_and_words_get_the_short_gap(self):
        rows = [{"clip_id": t, "text": t} for t in ("Armed", "Away", "Door")]
        by_text = {r["text"].lower(): r for r in rows}
        clips = {r["clip_id"]: tone(100, 0.3) for r in rows}
        lib = {"gap_ms": 60, "pause_ms": 300}
        y = record.joined(["Armed", "|", "Away", "Door"], by_text, clips, lib, RATE)
        self.assertEqual(len(y), 3 * len(clips["Armed"]) + RATE * (300 + 60) // 1000)


CUT = {"quiet_db": 35, "near_db": 6, "reach_ms": 250, "slack_ms": 20, "tail_ms": 10}


def row(cid, kind, category, text):
    return {"clip_id": cid, "kind": kind, "category": category, "text": text}


class CarrierTests(unittest.TestCase):
    """BL-202: a word is cut from a carrier sentence so it doesn't end like a sentence of its own
    (2026-10-09); the cut is recorded so `record --reproduce` repeats it."""

    LIB = {"carriers": {"position": "{} door is open.", "object": "Front {} tripped.",
                        "status.away": "Armed away.", "walk.walk_test": "Walk test ended."},
           "say": {"word.patio": "Pattio."}}

    def test_plan_finds_the_words_to_cut(self):
        plan = carrier.plan
        self.assertEqual(plan(row("word.front", "token", "position", "Front"), self.LIB),
                         ("Front door is open.", 0, 1))
        self.assertEqual(plan(row("word.carbon_monoxide", "token", "object", "Carbon monoxide"),
                              self.LIB), ("Front carbon monoxide tripped.", 1, 2))
        self.assertEqual(plan(row("word.patio", "token", "position", "Patio"), self.LIB),
                         ("Pattio door is open.", 0, 1))
        self.assertEqual(plan(row("status.away", "part", "status", "Away"), self.LIB),
                         ("Armed away.", 1, 1))
        self.assertEqual(plan(row("walk.walk_test", "part", "walk", "Walk test"), self.LIB),
                         ("Walk test ended.", 0, 2))

    def test_sentences_and_parts_without_a_carrier_are_spoken_as_they_are(self):
        self.assertIsNone(carrier.plan(row("status.exit_now", "part", "status",
                                           "You may exit now."), self.LIB))
        self.assertIsNone(carrier.plan(row("status.home", "part", "status", "Home"), self.LIB))
        lib = {"carriers": {"status": "Armed {}."}}  # a category's template is for words only
        self.assertIsNone(carrier.plan(row("status.home", "part", "status", "Home"), lib))
        with self.assertRaises(ValueError):
            carrier.plan(row("status.away", "part", "status", "Away"),
                         {"carriers": {"status.away": "Armed home."}})

    def test_the_cut_stops_at_the_silence_between_words(self):
        gap = [0.0] * (RATE * 60 // 1000)
        x = tone(200, 0.3) + gap + tone(300, 0.3, 300) + gap + tone(200, 0.3)
        # The middle word runs from 260 to 560 ms; the aligner says it starts 40 ms late.
        spans = [(0.0, 0.2), (0.3, 0.56), (0.62, 0.82)]
        a, b = carrier.cut_points(x, RATE, spans, 1, 1, CUT, ["front", "smoke", "tripped"])
        self.assertTrue(RATE * 200 // 1000 < a <= RATE * 260 // 1000, a)
        self.assertTrue(RATE * 560 // 1000 <= b < RATE * 620 // 1000, b)
        self.assertEqual(max(abs(v) for v in x[a - 40:a] + x[b:b + 40]), 0.0)

    def test_a_quiet_onset_with_no_silence_starts_where_the_word_before_ends(self):
        # "Front fence": the "f" is 26 dB under the vowel and follows "Front" with no silence.
        x = (tone(200, 0.3) + tone(60, 0.015, 3000) + tone(300, 0.3, 300)
             + [0.0] * (RATE * 60 // 1000) + tone(200, 0.3))
        spans = [(0.0, 0.2), (0.3, 0.56), (0.62, 0.82)]
        a, _ = carrier.cut_points(x, RATE, spans, 1, 1, CUT, ["front", "fence", "tripped"])
        self.assertEqual(a, RATE * 200 // 1000)

    def test_a_held_sound_is_kept_to_the_silence_after_it(self):
        # "Low contact": the aligner ends "low" at 250 ms, but its vowel goes on to 400 ms.
        x = [0.0] * (RATE // 10) + tone(300, 0.3) + [0.0] * (RATE * 40 // 1000) + tone(200, 0.3)
        spans = [(0.1, 0.25), (0.44, 0.6)]
        a, b = carrier.cut_points(x, RATE, spans, 0, 1, CUT, ["low", "contact"])
        self.assertTrue(a <= RATE // 10, a)
        self.assertTrue(RATE * 400 // 1000 <= b < RATE * 440 // 1000, b)

    def test_a_vowel_or_an_h_runs_into_its_neighbour_so_the_aligner_cuts(self):
        x = tone(300, 0.3) + tone(300, 0.3, 300) + tone(300, 0.3)
        spans = [(0.0, 0.3), (0.32, 0.6), (0.66, 0.9)]
        a, b = carrier.cut_points(x, RATE, spans, 1, 1, CUT, ["walk", "armed", "home"])
        self.assertEqual(a, RATE * 300 // 1000)            # where the previous word ends
        self.assertEqual(b, RATE * 610 // 1000)            # 10 ms after the aligner's end

    def test_a_word_at_the_edge_reaches_into_the_silence(self):
        x = [0.0] * (RATE // 5) + tone(300, 0.3) + [0.0] * (RATE // 5)
        a, b = carrier.cut_points(x, RATE, [(0.25, 0.5)], 0, 1, CUT, ["front"])
        self.assertTrue(RATE * 50 // 1000 <= a <= RATE * 200 // 1000, a)
        self.assertTrue(RATE * 500 // 1000 <= b <= RATE * 650 // 1000, b)

    def test_finish_makes_a_master_the_way_record_did_without_a_cut(self):
        x = [0.0] * 800 + tone(300, 0.2) + [0.0] * 800
        self.assertEqual(carrier.finish(x, RATE), ae.level(ae.trim(x, RATE), RATE))
        self.assertEqual(carrier.finish(x, RATE, (900, 4000)),
                         ae.level(ae.trim(x[900:4000], RATE), RATE))


class BenchTests(unittest.TestCase):
    """`voice.py try` (AUD-030): a phrase is joined from the script's words, the longest first."""

    WORDS = bench.vocabulary(LIB)

    def ids(self, phrase):
        return [r["clip_id"] for r in bench.parse(phrase, self.WORDS)]

    def test_a_phrase_is_its_words(self):
        self.assertEqual(self.ids("Back Patio Motion"), ["word.back", "word.patio", "word.motion"])
        self.assertEqual(self.ids("front door"), ["word.front", "word.door"])

    def test_a_compound_wins_over_its_parts(self):
        self.assertEqual(self.ids("Living Room Motion"), ["word.living_room", "word.motion"])

    def test_a_word_the_script_lacks_stops(self):
        with self.assertRaises(SystemExit):
            bench.parse("Front Drawbridge", self.WORDS)

    def test_a_whole_name_plays_only_at_the_end(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "back_door.wav").touch()
            words = bench.vocabulary(LIB, names=Path(d))
            ids = lambda p: [r["clip_id"] for r in bench.parse(p, words)]  # noqa: E731
            self.assertEqual(ids("Back Door"), ["name.back_door"])
            self.assertEqual(ids("Back Door Motion"), ["word.back", "word.door", "word.motion"])

    def test_forms_by_place_where_there_is_one(self):
        with tempfile.TemporaryDirectory() as d:
            for f in ("final/door.wav", "mid/back.wav"):
                (Path(d) / f).parent.mkdir(exist_ok=True)
                (Path(d) / f).touch()
            ids = lambda p: [r["clip_id"] for r in bench.with_forms(  # noqa: E731
                bench.parse(p, self.WORDS), Path(d))]
            self.assertEqual(ids("Back Door"), ["mid.back", "final.door"])
            self.assertEqual(ids("Front Door"), ["word.front", "final.door"])
            self.assertEqual(ids("Door Back"), ["word.door", "word.back"])


class EnhanceTests(unittest.TestCase):
    """The bench's low-volume enhancement (bench.py enhance()), as the panel would do it."""

    RATE = 16000

    def tone(self, hz, db, seconds=0.5):
        a = 10.0 ** (db / 20.0) * math.sqrt(2.0)
        return [a * math.sin(2.0 * math.pi * hz * i / self.RATE)
                for i in range(int(seconds * self.RATE))]

    def db(self, x):  # the last half: past the filters' and the detector's start
        x = x[len(x) // 2:]
        return 10.0 * math.log10(sum(s * s for s in x) / len(x))

    def test_the_presence_lift_is_its_gain_at_3_khz_and_none_far_away(self):
        coeffs = bench.peaking(self.RATE, bench.PRESENCE_HZ, bench.PRESENCE_Q, 6.0)
        for hz, gain in ((3000, 6.0), (300, 0.0)):
            x = self.tone(hz, -20.0)
            self.assertAlmostEqual(self.db(bench.biquad(x, coeffs)) - self.db(x), gain, delta=0.3)

    def test_the_high_pass_cuts_the_boom_and_keeps_the_voice(self):
        coeffs = bench.highpass(self.RATE, 180.0)
        low, voice = self.tone(50, -20.0), self.tone(1000, -20.0)
        self.assertLess(self.db(bench.biquad(low, coeffs)) - self.db(low), -20.0)
        self.assertAlmostEqual(self.db(bench.biquad(voice, coeffs)) - self.db(voice), 0.0, delta=0.3)

    def test_soft_sounds_come_up_and_the_quiet_between_words_does_not(self):
        ratio, voice = 2.5, -10.0
        makeup = 15.0 * (1.0 - 1.0 / ratio)
        for under, lift in ((0.0, 0.0), (20.0, makeup), (60.0, 0.0)):
            x = self.tone(1000, voice - under)
            got = self.db(bench.compress(x, self.RATE, voice, ratio)) - self.db(x)
            self.assertAlmostEqual(got, lift, delta=1.0, msg=f'{under} dB under the voice')


bench_takes = load("bench_takes")


class BenchTakesTests(unittest.TestCase):
    """`voice.py bench-takes`: the joins research's trials, cut from the right sentences."""

    ALONE = {"back": 0.46, "front": 0.61, "garage": 0.68, "kitchen": 0.57, "patio": 0.62,
             "door": 0.5, "window": 0.64, "motion": 0.63}

    def test_each_cut_is_its_words_in_its_sentence(self):
        for _, text, cuts in bench_takes.sentences(LIB):
            words = bench_takes.carrier.words(text)
            for kind, key, first, count in cuts:
                said = " ".join(words[first:first + count]).replace("pattio", "patio")
                self.assertEqual(said, key, text)

    def test_final_forms_end_an_announcement_and_mid_forms_do_not(self):
        for _, text, cuts in bench_takes.sentences(LIB):
            n = len(bench_takes.carrier.words(text))
            for kind, key, first, count in cuts:
                if kind == "final":
                    self.assertTrue(text.startswith(bench_takes.LEAD) and first + count == n)
                if kind == "mid":
                    self.assertLess(first + count, n, text)

    def test_a_clean_start_beats_the_length(self):
        # A cut that starts mid-sound plays choppy ("Back patio motion", 2026-10-09).
        chopped = {"file": "a", "seconds": 0.59, "onset_db": -2.0}
        clean = {"file": "b", "seconds": 0.70, "onset_db": -60.0}
        best = min((chopped, clean), key=lambda c: bench_takes.rank(c, 0.55, 0.63, {}))
        self.assertIs(best, clean)
        misheard = {"file": "c", "seconds": 0.59, "onset_db": -60.0}
        best = min((misheard, clean), key=lambda c: bench_takes.rank(c, 0.55, 0.63, {"c": False}))
        self.assertIs(best, clean)

    def test_last_words_are_cut_after_a_stop(self):
        finals = [(text, first) for _, text, cuts in bench_takes.sentences(LIB)
                  for kind, _, first, _ in cuts if kind == "final"]
        self.assertEqual(len(finals), len(bench_takes.FINAL))
        for text, first in finals:
            self.assertEqual(bench_takes.carrier.words(text)[first - 1], "deck", text)

    def test_targets(self):
        t = lambda *k: bench_takes.target(*k, self.ALONE)  # noqa: E731
        self.assertEqual(t("alone", "back door"), (0.80, 0.90))  # the research's own
        lo, hi = t("mid", "front")
        self.assertLess(hi, self.ALONE["front"])                 # shorter than spoken alone
        lo, hi = t("leadin", "front door")
        self.assertGreater(lo, t("mid", "front")[0] + t("final", "door")[0])  # and the join


class CheckTests(unittest.TestCase):
    def test_words_that_sound_alike_count_as_heard(self):
        self.assertTrue(asr_check.heard_as("Maine.", "Main", "word.main"))
        self.assertTrue(asr_check.heard_as("Armed Knight. No entry delay.",
                                           "Armed night. No entry delay.", None))

    def test_a_number_may_come_back_as_digits(self):
        self.assertTrue(asr_check.heard_as("12.", "Twelve", "number.12"))
        self.assertFalse(asr_check.heard_as("12.", "Twelve", "number.20"))

    def test_another_word_is_a_miss(self):
        self.assertFalse(asr_check.heard_as("Paytio.", "Patio", "word.patio"))
        self.assertFalse(asr_check.heard_as("Space.", "Base", "extra.base"))

    def test_a_take_or_an_ending_of_a_number_may_come_back_as_digits(self):
        self.assertTrue(asr_check.heard_as("2.", "Two", "number.2@s1234"))
        self.assertTrue(asr_check.heard_as("2.", "Two", "number.2.end"))
        self.assertFalse(asr_check.heard_as("2.", "Two", "number.20@s1234"))


forms = load("forms")


class FormsTests(unittest.TestCase):
    """AUD-031: `voice.py forms` speaks each word's own clip (its form inside a name) and its
    ending (its form at the end of one) in carrier sentences, and picks a take of each."""

    def test_each_word_is_cut_inside_a_name_and_each_ending_at_the_end(self):
        lead = carrier.words("Not ready. Check the zones.")
        seen = set()
        for text, cuts in forms.plan(FORMS_LIB).items():
            words = carrier.words(text)
            for r, first, count in cuts:
                seen.add(r["clip_id"])
                said = words[first:first + count]
                written = carrier.words(FORMS_LIB["say"].get(r.get("base", r["clip_id"]), r["text"]))
                self.assertIn(said, (written, carrier.words(r["text"])), text)
                if r["kind"] == "ending":
                    self.assertEqual(words[:len(lead)], lead, text)
                    self.assertEqual(first + count, len(words), text)
                else:
                    self.assertLess(first + count, len(words), text)
        wanted = {r["clip_id"] for r in record.clips_wanted(FORMS_LIB) if r["kind"] in forms.KINDS}
        self.assertEqual(seen, wanted)
        only = forms.plan(FORMS_LIB, only={"word.door", "word.door.end"})
        self.assertEqual(sorted(r["clip_id"] for cuts in only.values() for r, _, _ in cuts),
                         ["word.door", "word.door.end"])

    def test_a_form_is_named_by_its_word(self):
        self.assertEqual(forms.key_of({"clip_id": "word.living_room.end",
                                       "base": "word.living_room"}), "living_room")
        self.assertEqual(forms.key_of({"clip_id": "number.2"}), "2")
        self.assertEqual(forms.form_of({"kind": "ending"}), "final")
        self.assertEqual(forms.form_of({"kind": "token"}), "mid")

    def test_an_ending_is_nearly_as_long_as_the_word_alone_and_a_word_shorter(self):
        alone = {"garage": 0.68, "front": 0.61}
        lo, hi = forms.target("final", "garage", alone)
        self.assertAlmostEqual(lo, 0.578)
        self.assertEqual(hi, 0.68)
        lo, hi = forms.target("mid", "front", alone)
        self.assertLess(hi, alone["front"])
        self.assertEqual(forms.target("final", "door", alone), forms.TARGETS[("final", "door")])

    def test_a_word_of_one_syllable_shortens_more_inside_a_name(self):
        alone = {"side": 0.57, "zone": 0.69, "2": 0.46, "garage": 0.68, "foyer": 0.62}
        for key, said in (("side", "Side"), ("zone", "Zone"), ("2", "two")):
            lo, hi = forms.target("mid", key, alone, said)
            self.assertAlmostEqual(lo, alone[key] * 0.6, msg=key)
            self.assertAlmostEqual(hi, alone[key] * 0.75, msg=key)
        for key, said in (("garage", "Garage"), ("foyer", "Foyer")):
            lo, hi = forms.target("mid", key, alone, said)
            self.assertAlmostEqual(lo, alone[key] * 0.72, msg=key)
            self.assertAlmostEqual(hi, alone[key] * 0.82, msg=key)
        self.assertEqual([forms.syllables(w) for w in ("Suite", "Fire", "Twelve", "Living room")],
                         [1, 1, 1, 3])

    def test_a_take_whisper_heard_ranks_before_one_not_checked(self):
        heard_ok = {"file": "a", "seconds": 0.30, "onset_db": -40.0}
        unchecked = {"file": "b", "seconds": 0.42, "onset_db": -40.0}
        misheard = {"file": "c", "seconds": 0.42, "onset_db": -40.0}
        heard = {"a": True, "c": False}
        best = min((unchecked, misheard, heard_ok), key=lambda c: forms.rank(c, 0.4, 0.45, heard))
        self.assertIs(best, heard_ok)
        best = min((misheard, unchecked), key=lambda c: forms.rank(c, 0.4, 0.45, heard))
        self.assertIs(best, unchecked)

    def test_the_budget_shortens_a_long_word_before_a_short_one(self):
        def take(s):
            return {"seconds": s}
        long_, short = [take(1.0), take(0.9)], [take(0.4), take(0.36)]
        chosen = {"long": long_[0], "short": short[0]}
        options = {"long": (1.0, long_), "short": (0.4, short)}
        room = forms.pack_bytes(0.95) + forms.pack_bytes(0.4)
        fitted, used = forms.fit(chosen, options, room)
        self.assertIs(fitted["long"], long_[1])
        self.assertIs(fitted["short"], short[0])
        self.assertLessEqual(used, room)
        fitted, used = forms.fit(chosen, options, forms.pack_bytes(1.0))  # can't fit
        self.assertGreater(used, forms.pack_bytes(1.0))
        self.assertEqual((fitted["long"], fitted["short"]), (long_[1], short[1]))

    def test_a_part_restarted_alone_keeps_its_own_share(self):
        todo, wanted = ["A.", "B.", "C."], [1, 2]
        p0, p1 = forms.share(todo, wanted, set(), (0, 2)), forms.share(todo, wanted, set(), (1, 2))
        self.assertEqual(sorted(p0 + p1), sorted((t, s) for t in todo for s in wanted))
        done = set(p0[:2]) | set(p1[:1])  # p0 ran further than p1 before p1 stopped
        self.assertEqual(forms.share(todo, wanted, done, (1, 2)), p1[1:])
        self.assertEqual(forms.share(["A."], wanted, {("A.", 1), ("B.", 1)}, (0, 1), again=True),
                         [("A.", 1), ("A.", 2)])

    def test_speaking_again_removes_the_old_takes_from_every_part(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            for home, seed in ((out, 1), (out / "p0", 2)):
                (home / "takes" / "zone_two_door").mkdir(parents=True)
                (home / "takes" / "back_den_door").mkdir(parents=True)
                (home / "manifest.json").write_text(json.dumps({"clips": {
                    f"number.2@s{seed}": {"clip_id": "number.2", "seed": seed,
                                          "sentence": "Zone two door.", "file": "a.wav"},
                    f"word.den@s{seed}": {"clip_id": "word.den", "seed": seed,
                                          "sentence": "Back den door.", "file": "b.wav"}}}),
                                                     encoding="utf-8")
            forms.forget(out, {"Zone two door."})
            self.assertEqual(sorted(forms.manifests(out)), ["word.den@s1", "word.den@s2"])
            self.assertFalse((out / "p0" / "takes" / "zone_two_door").exists())
            self.assertTrue((out / "p0" / "takes" / "back_den_door").exists())

    def test_takes_of_every_part_are_read_back(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            for part, seed in (("p0", 1), ("p1", 2)):
                (out / part / "word.door").mkdir(parents=True)
                (out / part / "manifest.json").write_text(json.dumps({"clips": {
                    f"word.door@s{seed}": {"clip_id": "word.door", "seed": seed,
                                           "file": f"word.door/s{seed}.wav"}}}),
                                                          encoding="utf-8")
            takes = forms.manifests(out)
        self.assertEqual(sorted(takes), ["word.door@s1", "word.door@s2"])
        self.assertEqual(takes["word.door@s2"]["file"], "p1/word.door/s2.wav")


tracked = load("tracked")
vp = tracked.vp            # the voicepack module tracked.py uses (one FormatError class)
REPO = VOICE.parents[1]
SCRIPT = REPO / LIB["script"]
REAL_VOICE = tracked.voice_dir(LIB)
VOICE_IDS_H = REPO / "components/core/voice_pack/include/voice_pack/voice_ids.h"
# id -> clip_id as recording-script.csv assigns them today (D07): never renumber or reuse.
REGISTRY_SHA256 = "1f78846c25b8b89c6a3f6b8a974d2d072d4dd1df1dfd48f6032ec86ab0a3351e"
# The unsigned payload of pack revision 9 (accepted, not a draft) built from the tracked en-US
# voice: a new revision updates both.
GOLDEN_REVISION = 9
GOLDEN_PAYLOAD_SHA256 = "e32f5f37f79c08e7e26478e3eda55daa8b95837a48981a672960020e2639aaab"

_spec = importlib.util.spec_from_file_location("voice_i18n", REPO / "tools" / "i18n.py")
i18n = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(i18n)

SILK_20MS = 0x48           # TOC: config 9 (SILK wideband, 20 ms), mono, code 0
PRE48 = 312                # 104 samples at 16 kHz, as libopus gives at 16 kHz


def opus_head(pre48=PRE48, channels=1, rate=16000):
    return b"OpusHead" + struct.pack("<BBHIhB", 1, channels, pre48, rate, 0, 0)


def opus_ogg(packets, granule, pre48=PRE48, head=None, tags=b"OpusTags" + bytes(8)):
    """A small Ogg Opus file: OpusHead and OpusTags on their own pages, then two packets a page."""
    pages = [vp.ogg_page([(head or opus_head(pre48), True)], 0, 7, 0, 0x2),
             vp.ogg_page([(tags, True)], 0, 7, 1, 0)]
    for i in range(0, len(packets), 2):
        last = i + 2 >= len(packets)
        pages.append(vp.ogg_page([(p, True) for p in packets[i:i + 2]], granule if last else 0,
                                 7, len(pages), 0x4 if last else 0))
    return b"".join(pages)


def fake_packets(n, size=40, toc=SILK_20MS, seed=0):
    return [bytes([toc]) + bytes((seed + i + j) & 0xFF for j in range(size - 1))
            for i in range(n)]


def packets_for(samples, pre=PRE48 // 3):
    return -(-(samples + pre) // 320)


def write_wav(path, samples, rate=24000, amp=1000):
    pcm = array.array("h", (amp * ((i % 7) - 3) for i in range(samples)))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def synthetic_rows():
    rows = [("word.front", 1, "token", "position", "Front"),
            ("word.door", 2, "token", "object", "Door")]
    rows += [(cid, nid, "part", cid.split(".")[0], cid.split(".")[1].replace("_", " ").title()
              + ".") for cid, nid in vp.REQUIRED_IDS.items()]
    return rows


def write_script(path, rows):
    lines = ["clip_id,numeric_id,kind,category,text,planning_seconds,notes"]
    lines += [f"{c},0x{n:04X},{k},{cat},{t},0.75," for c, n, k, cat, t in rows]
    path.write_bytes(("\n".join(lines) + "\n").encode())


def synthetic_lib(**over):
    lib = dict(LIB, pack_revision=1)
    lib.update(over)
    return lib


def make_recording(src, rows, lib, granule_form="ffmpeg"):
    """A recording folder as voice.py record and encode leave it, for a tiny script."""
    (src / f"opus{lib['kbps']}").mkdir(parents=True)
    write_wav(src / "reference.wav", 2400)
    manifest = {"voice": {"id": lib["voice"], "kind": "design", "design": "test"}, "clips": {}}
    lengths = {}
    for i, r in enumerate(rows):
        n = 2400 + 30 * i                      # 24 kHz master; the encoder gets two thirds
        write_wav(src / f"{r['clip_id']}.wav", n)
        lengths[r["clip_id"]] = n * 2 // 3
        granule = (2 * PRE48 if granule_form == "ffmpeg" else PRE48) + 3 * lengths[r["clip_id"]]
        ogg = opus_ogg(fake_packets(packets_for(lengths[r["clip_id"]]), seed=i), granule)
        (src / f"opus{lib['kbps']}" / f"{r['clip_id']}.opus").write_bytes(ogg)
        manifest["clips"][r["clip_id"]] = {"seed": 1234, "text": r["text"], "spoken": r["spoken"]}
    (src / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    (src / "provenance.json").write_text(json.dumps(
        {"engine": "qwen3", "models": {"m": "rev"}, "packages": {}, "gpu": "g",
         "made": "2026-10-08 00:00"}), encoding="utf-8")
    return lengths


class SyntheticVoice:
    """A temporary script, recording and tracked folder (two tokens and the 16 fixed clips)."""

    def __init__(self, **lib_over):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.lib = synthetic_lib(**lib_over)
        write_script(root / "script.csv", synthetic_rows())
        self.rows, problems = tracked.read_registry(root / "script.csv", self.lib)
        assert not problems, problems
        self.src, self.dst = root / "rec", root / "voice"
        self.lengths = make_recording(self.src, self.rows, self.lib)

    def export(self):
        return tracked.export(self.src, self.dst, self.rows, self.lib, self.lengths)

    def verify(self):
        return tracked.verify(self.dst, self.rows, self.lib)

    def meta(self):
        return tracked.load_meta(self.dst)

    def save_meta(self, meta):
        (self.dst / "voice.json").write_bytes(tracked.json_bytes(meta))

    def close(self):
        self.tmp.cleanup()


def payload_clips(n_tokens=2):
    clips = []
    for i, (cid, nid) in enumerate([(f"word.t{i}", i + 1) for i in range(n_tokens)]
                                   + list(vp.REQUIRED_IDS.items())):
        cat = cid.split(".")[0] if nid >= 0x8000 else "object"
        valid = 600 + i
        packets = fake_packets(packets_for(valid), size=20 + i, seed=i)
        clips.append(dict(numeric_id=nid, category=cat, text=f"Clip {i}", key=cid,
                          data=vp.frame_packets(packets), valid_samples=valid,
                          packet_count=len(packets), pre_skip=104))
    return clips


PACK_ARGS = dict(revision=7, draft=True, locale="en-US", voice_id="qwen3-designed_female",
                 word_gap_ms=60, part_gap_ms=300, nominal_bitrate=32000)


def sbv2_sector(signed_bytes):
    """A signature sector shaped like espsecure's (made-up RSA fields, real digest and CRC)."""
    sector = bytearray(b"\xff" * 4096)
    sector[0:4] = b"\xe7\x02\x00\x00"
    sector[4:36] = hashlib.sha256(signed_bytes).digest()
    struct.pack_into("<I", sector, 1196, zlib.crc32(bytes(sector[:1196])))
    return bytes(sector)


class RegistryTests(unittest.TestCase):
    """AUD-026: recording-script.csv's numeric_id column is the registry; IDs never change."""

    def setUp(self):
        self.rows, self.problems = tracked.read_registry(SCRIPT, LIB)

    def test_todays_ids_are_pinned(self):
        self.assertEqual(self.problems, [])
        pairs = "".join(f"0x{r['numeric_id']:04X} {r['clip_id']}\n" for r in self.rows
                        if r["category"] != "ending")
        self.assertEqual(hashlib.sha256(pairs.encode()).hexdigest(), REGISTRY_SHA256,
                         "a numeric ID changed: IDs are never renumbered or reused (D07)")
        ids = {r["clip_id"]: r["numeric_id"] for r in self.rows}
        self.assertEqual(ids["word.front"], 1)
        self.assertEqual(ids["word.back"], 2)
        self.assertEqual(ids["number.100"], 221)
        self.assertEqual(ids["status.arming"], 0x8001)
        self.assertEqual(ids["status.not_ready"], 0x800A)
        self.assertEqual(ids["walk.walk_test"], 0x8101)
        self.assertEqual(ids["walk.some_untested"], 0x8106)
        tokens = [r["numeric_id"] for r in self.rows if r["kind"] == "token"]
        self.assertEqual(tokens, list(range(1, 222)))

    def test_the_sixteen_fixed_ids_are_d07s(self):
        ids = {r["clip_id"]: r["numeric_id"] for r in self.rows}
        self.assertEqual({k: ids[k] for k in vp.REQUIRED_IDS}, vp.REQUIRED_IDS)

    def test_a_broken_registry_is_refused(self):
        def problems(edit):
            rows = [dict(r) for r in self.rows]
            edit(rows)
            return tracked.registry_problems(rows)
        self.assertEqual(problems(lambda rows: None), [])
        dup = problems(lambda rows: rows[1].update(numeric_id=1))
        self.assertTrue(any("also word.front" in p for p in dup), dup)
        outside = problems(lambda rows: rows[0].update(numeric_id=0x8100))
        self.assertTrue(any("outside the position namespace" in p for p in outside), outside)
        status = problems(lambda rows: next(r for r in rows if r["clip_id"] == "status.home")
                          .update(numeric_id=0x800B))
        self.assertTrue(any("D07 makes it 0x8004" in p for p in status), status)
        gone = problems(lambda rows: rows.remove(next(r for r in rows
                                                      if r["clip_id"] == "walk.ended")))
        self.assertTrue(any("walk.ended is missing" in p for p in gone), gone)
        with tempfile.TemporaryDirectory() as d:
            write_script(Path(d) / "s.csv", [("word.front", 1, "token", "position", "Front")])
            text = (Path(d) / "s.csv").read_text(encoding="utf-8").replace("0x0001", "1")
            (Path(d) / "s.csv").write_text(text, encoding="utf-8")
            _, p = tracked.read_registry(Path(d) / "s.csv", LIB)
        self.assertTrue(any("not 0x and four hex digits" in x for x in p), p)

    def test_the_c_constants_match_the_registry(self):
        if not VOICE_IDS_H.exists():
            self.skipTest("voice_ids.h is not written yet (the core voice_pack reader adds it)")
        defines = {}
        for line in VOICE_IDS_H.read_text(encoding="utf-8").splitlines():
            m = re.match(r"\s*#define\s+VP_ID_(\w+)\s+(0x[0-9A-Fa-f]+)u?\b", line)
            if m:
                defines[m.group(1)] = int(m.group(2), 16)
        fixed = {r["clip_id"]: r for r in self.rows if r["category"] in ("status", "walk")}
        self.assertEqual({v for v in defines.values() if v >= 0x8000},
                         {r["numeric_id"] for r in fixed.values()})
        for cid, r in fixed.items():
            cat, name = (s.upper() for s in cid.split("."))
            names = {n for n in (name, f"{cat}_{name}") if n in defines}
            self.assertTrue(names, f"no VP_ID_ constant for {cid}")
            for n in names:
                self.assertEqual(defines[n], r["numeric_id"], f"VP_ID_{n} vs {cid}")


class OggOpusTests(unittest.TestCase):
    """AUD-023: the encoder's Ogg Opus files become clip data, checked as D08 requires."""

    def test_a_hand_made_stream_gives_its_packets_and_trim(self):
        packets = fake_packets(4)
        ogg = opus_ogg(packets, PRE48 + 3 * 1000)
        got, pre, valid = vp.opus_stream(ogg)
        self.assertEqual((got, pre, valid), (packets, 104, 1000))

    def test_a_packet_continued_on_the_next_page_is_joined(self):
        big = bytes([SILK_20MS]) + bytes(range(256)) * 2 + bytes(87)    # 600 bytes
        pages = [vp.ogg_page([(opus_head(), True)], 0, 1, 0, 0x2),
                 vp.ogg_page([(b"OpusTags" + bytes(8), True)], 0, 1, 1, 0),
                 vp.ogg_page([(fake_packets(1)[0], True), (big[:510], False)], -1, 1, 2, 0),
                 vp.ogg_page([(big[510:], True)], PRE48 + 3 * 500, 1, 3, 0x1 | 0x4)]
        got, pre, valid = vp.opus_stream(b"".join(pages))
        self.assertEqual(got, [fake_packets(1)[0], big])
        self.assertEqual((pre, valid), (104, 500))
        broken = pages[:3] + [vp.ogg_page([(big[510:], True)], PRE48 + 3 * 500, 1, 3, 0x4)]
        with self.assertRaisesRegex(vp.FormatError, "continued-packet flag"):
            vp.ogg_packets(b"".join(broken))

    def test_ffmpegs_granule_is_checked_against_the_input_length(self):
        packets = fake_packets(packets_for(1000))
        ffmpeg = opus_ogg(packets, 2 * PRE48 + 3 * 1000)
        self.assertEqual(vp.opus_stream(ffmpeg, input_samples=1000)[2], 1000)
        self.assertEqual(vp.opus_stream(opus_ogg(packets, PRE48 + 3 * 1000), 1000)[2], 1000)
        with self.assertRaisesRegex(vp.FormatError, "does not match 999 input"):
            vp.opus_stream(ffmpeg, input_samples=999)

    def test_damaged_or_cut_off_files_are_refused(self):
        ogg = bytearray(opus_ogg(fake_packets(4), PRE48 + 3 * 1000))
        with self.assertRaisesRegex(vp.FormatError, "cut off"):
            vp.ogg_packets(bytes(ogg[:-5]))
        ogg[-3] ^= 1
        with self.assertRaisesRegex(vp.FormatError, "checksum"):
            vp.ogg_packets(bytes(ogg))
        no_eos = opus_ogg(fake_packets(4), PRE48 + 3 * 1000)
        last = no_eos.rfind(b"OggS")
        page = bytearray(no_eos[last:])
        page[5] = 0
        page[22:26] = b"\0\0\0\0"
        struct.pack_into("<I", page, 22, vp.ogg_crc(page))
        with self.assertRaisesRegex(vp.FormatError, "end-of-stream"):
            vp.ogg_packets(no_eos[:last] + bytes(page))

    def test_trim_must_divide_into_16_khz_samples(self):
        with self.assertRaisesRegex(vp.FormatError, "pre-skip 313"):
            vp.opus_stream(opus_ogg(fake_packets(4), 313 + 3000, pre48=313))
        with self.assertRaisesRegex(vp.FormatError, "final granule"):
            vp.opus_stream(opus_ogg(fake_packets(4), PRE48 + 3001))

    def test_only_single_20_ms_frames_are_accepted(self):
        with self.assertRaisesRegex(vp.FormatError, "TOC code 1"):
            vp.opus_stream(opus_ogg(fake_packets(4, toc=SILK_20MS | 1), PRE48 + 3000))
        with self.assertRaisesRegex(vp.FormatError, "10 ms frame"):
            vp.opus_stream(opus_ogg(fake_packets(4, toc=0x40), PRE48 + 3000))
        self.assertEqual(vp.opus_frame_samples(0xF8), 320)     # CELT fullband 20 ms
        self.assertEqual(vp.opus_frame_samples(0x68), 320)     # hybrid 20 ms

    def test_packets_must_cover_the_trim_with_less_than_a_packet_over(self):
        self.assertIn("decoded", " ".join(vp.check_packets(fake_packets(3), 104, 900)))
        self.assertIn("whole packet", " ".join(vp.check_packets(fake_packets(5), 104, 900)))
        self.assertEqual(vp.check_packets(fake_packets(4), 104, 900), [])
        self.assertIn("pre-skip 321", " ".join(vp.check_packets(fake_packets(5), 321, 900)))
        self.assertIn("160001", " ".join(vp.check_packets(fake_packets(501), 0, 160001)))

    def test_not_mono_opus_is_refused(self):
        with self.assertRaisesRegex(vp.FormatError, "2 channels"):
            vp.opus_stream(opus_ogg(fake_packets(4), PRE48 + 3000, head=opus_head(channels=2)))
        with self.assertRaisesRegex(vp.FormatError, "OpusTags"):
            vp.opus_stream(opus_ogg(fake_packets(4), PRE48 + 3000, tags=b"Nope" + bytes(12)))

    def test_framing_round_trips_and_refuses_a_cut(self):
        packets = fake_packets(3, size=33)
        data = vp.frame_packets(packets)
        self.assertEqual(data[:2], struct.pack("<H", 33))
        self.assertEqual(vp.unframe(data), packets)
        with self.assertRaisesRegex(vp.FormatError, "cut off"):
            vp.unframe(data[:-1])
        with self.assertRaisesRegex(vp.FormatError, "length 0"):
            vp.unframe(data + b"\0\0")


class PayloadTests(unittest.TestCase):
    """AUD-026: RVOC format 1 is laid out byte for byte as D08 says, and refuses what it must."""

    def setUp(self):
        self.clips = payload_clips()
        self.payload = vp.build_payload(self.clips, **PACK_ARGS)

    def u(self, fmt, off, data=None):
        return struct.unpack_from("<" + fmt, data or self.payload, off)[0]

    def test_every_header_field_is_at_its_offset(self):
        p, n = self.payload, len(self.clips)
        self.assertEqual(p[0:4], b"RVOC")
        self.assertEqual((self.u("H", 4), self.u("H", 6)), (1, 128))
        self.assertEqual(self.u("I", 8), len(p))
        self.assertEqual(self.u("I", 12), 7)
        self.assertEqual(self.u("I", 16), 1)
        self.assertEqual(self.u("I", 20), 1)                          # DRAFT
        self.assertEqual(p[24:40], b"en-US".ljust(16, b"\0"))
        self.assertEqual(p[40:72], b"qwen3-designed_female".ljust(32, b"\0"))
        self.assertEqual(self.u("I", 72), 16000)
        self.assertEqual((self.u("H", 76), p[78], p[79]), (1, 1, 20))
        self.assertEqual((self.u("H", 80), self.u("H", 82)), (n, 64))
        self.assertEqual(self.u("I", 84), 128)
        strings = self.u("I", 88)
        self.assertEqual(strings, 128 + 64 * n)
        audio = self.u("I", 96)
        self.assertEqual(audio, (strings + self.u("I", 92) + 3) & ~3)
        self.assertEqual(audio + self.u("I", 100), len(p))
        self.assertEqual((self.u("H", 104), self.u("H", 106)), (60, 300))
        self.assertEqual(self.u("I", 108), 32000)
        self.assertEqual(p[112:128], bytes(16))
        released = vp.build_payload(self.clips, **dict(PACK_ARGS, draft=False))
        self.assertEqual(self.u("I", 20, released), 0)

    def test_index_entries_are_sorted_and_point_at_their_clip(self):
        p = self.payload
        by_key = {c["key"]: c for c in self.clips}
        ids = [self.u("H", 128 + 64 * i) for i in range(len(self.clips))]
        self.assertEqual(ids, sorted(c["numeric_id"] for c in self.clips))
        for i in range(len(self.clips)):
            e = 128 + 64 * i
            key_at = self.u("I", e + 8)
            key = p[key_at:p.index(b"\0", key_at)].decode()
            c = by_key[key]
            self.assertEqual(self.u("H", e), c["numeric_id"])
            self.assertEqual(p[e + 2], vp.CATEGORIES[c["category"]])
            self.assertEqual(p[e + 3], 0)
            text_at = self.u("I", e + 4)
            self.assertEqual(p[text_at:p.index(b"\0", text_at)].decode(), c["text"])
            self.assertEqual(text_at + len(c["text"]) + 1, key_at)   # text, then its key
            at, length = self.u("I", e + 12), self.u("I", e + 16)
            self.assertEqual(at % 4, 0)
            self.assertEqual(p[at:at + length], c["data"])
            self.assertEqual(self.u("I", e + 20), c["valid_samples"])
            self.assertEqual(self.u("H", e + 24), c["packet_count"])
            self.assertEqual(self.u("H", e + 26), 104)
            self.assertEqual(p[e + 28:e + 60], hashlib.sha256(c["data"]).digest())
            self.assertEqual(p[e + 60:e + 64], bytes(4))

    def test_alignment_bytes_are_zero_and_the_reader_accepts_it(self):
        header, entries = vp.parse_payload(self.payload)
        self.assertEqual(header["pack_revision"], 7)
        self.assertEqual(len(entries), len(self.clips))
        starts = [e["data_offset"] for e in entries]
        ends = [e["data_offset"] + e["data_length"] for e in entries]
        for end, nxt in zip(ends, starts[1:]):
            self.assertLess(nxt - end, 4)
            self.assertEqual(self.payload[end:nxt], bytes(nxt - end))

    def test_the_same_input_gives_the_same_bytes(self):
        again = vp.build_payload(list(reversed(payload_clips())), **PACK_ARGS)
        self.assertEqual(again, self.payload)

    def test_a_broken_set_is_refused(self):
        def refused(edit, msg):
            clips = payload_clips()
            edit(clips)
            with self.assertRaisesRegex(vp.FormatError, msg):
                vp.build_payload(clips, **PACK_ARGS)
        refused(lambda c: c[1].update(numeric_id=1), "unique")
        refused(lambda c: c[1].update(key="word.t0"), "unique")
        refused(lambda c: c[0].update(numeric_id=0x8000), "outside the object namespace")
        refused(lambda c: c[0].update(category="status"), "outside the status namespace")
        refused(lambda c: c[0].update(text="x" * 48), "display text")
        refused(lambda c: c[0].update(text="Front\nDoor"), "display text")
        refused(lambda c: c[0].update(key="word front"), "script key")
        refused(lambda c: c[0].update(packet_count=9), "counted")
        refused(lambda c: c[0].update(valid_samples=5000), "decoded")
        with self.assertRaisesRegex(vp.FormatError, "clips"):
            vp.build_payload([], **PACK_ARGS)
        with self.assertRaisesRegex(vp.FormatError, "revision"):
            vp.build_payload(payload_clips(), **dict(PACK_ARGS, revision=0))

    def test_the_reader_refuses_malformed_payloads(self):
        def refused(offset, value, msg):
            p = bytearray(self.payload)
            p[offset] = value
            with self.assertRaisesRegex(vp.FormatError, msg):
                vp.parse_payload(bytes(p))
        refused(112, 1, "reserved header")
        refused(20, 3, "does not allow")
        refused(128 + 3, 1, "reserved bytes")
        refused(128 + 64 + 0, 0, "does not ascend")          # entry 1's ID below entry 0's
        refused(128 + 2, 6, "category")
        refused(len(self.payload) - 1, self.payload[-1] ^ 1, "SHA-256")
        text_at = self.u("I", 128 + 4)
        refused(text_at, 0x07, "display text")
        p = bytearray(self.payload)
        struct.pack_into("<I", p, 128 + 12, len(p) - 2)      # clip data past the end
        with self.assertRaisesRegex(vp.FormatError, "misplaced"):
            vp.parse_payload(bytes(p))
        with self.assertRaisesRegex(vp.FormatError, "payload_length"):
            vp.parse_payload(self.payload + b"\0")

    def test_a_pack_without_a_fixed_clip_is_refused(self):
        clips = [c for c in payload_clips() if c["key"] != "walk.ended"]
        with self.assertRaisesRegex(vp.FormatError, "missing walk.ended"):
            vp.parse_payload(vp.build_payload(clips, **PACK_ARGS))

    def test_the_signed_envelope(self):
        p = self.payload
        unsigned = i18n.unsigned_pack_file(p)
        self.assertEqual(vp.check_envelope(unsigned, signed=False), p)
        with self.assertRaisesRegex(vp.FormatError, "no Secure Boot v2"):
            vp.check_envelope(unsigned)
        body = unsigned[:-4096]
        self.assertEqual(vp.check_envelope(body + sbv2_sector(body)), p)
        with self.assertRaisesRegex(vp.FormatError, "signs other bytes"):
            vp.check_envelope(body + sbv2_sector(body[:-1] + b"\0"))
        bad_pad = bytearray(unsigned)
        bad_pad[len(p)] = 0
        with self.assertRaisesRegex(vp.FormatError, "0xFF"):
            vp.check_envelope(bytes(bad_pad), signed=False)
        with self.assertRaisesRegex(vp.FormatError, "multiple of 4096"):
            vp.check_envelope(unsigned[:-1], signed=False)
        with self.assertRaisesRegex(vp.FormatError, "multiple of 4096"):
            vp.check_envelope(unsigned + b"\xff" * (970752 + 4096 - len(unsigned)), signed=False)
        longer = unsigned + b"\xff" * 4096                  # one sector of padding too many
        with self.assertRaisesRegex(vp.FormatError, "does not fill"):
            vp.check_envelope(longer, signed=False)
        self.assertEqual(vp.signed_size(4096), 8192)
        self.assertEqual(vp.signed_size(4097), 12288)
        self.assertEqual(vp.signed_size(vp.PAYLOAD_MAX), vp.FILE_MAX)


FIXTURE_V2_H = REPO / "components/core/voice_pack/test/voice_pack_fixture_v2.h"


class EndingPayloadTests(unittest.TestCase):
    """AUD-031: a pack with endings is vocabulary schema 2; an ending lives at 0xC000 + its
    word's ID and needs its word in the same pack."""

    def clips(self, word_id=2):
        clips = payload_clips()
        return clips + [dict(clips[1], numeric_id=vp.ending_id(word_id), category="ending",
                             key="word.t1.end")]

    def test_endings_make_a_schema_2_pack_the_reader_takes(self):
        self.assertEqual(struct.unpack_from("<I", vp.build_payload(payload_clips(), **PACK_ARGS),
                                            16)[0], vp.VOCABULARY_SCHEMA)
        payload = vp.build_payload(self.clips(), **PACK_ARGS)
        self.assertEqual(struct.unpack_from("<I", payload, 16)[0], vp.VOCABULARY_SCHEMA_ENDINGS)
        header, entries = vp.parse_payload(payload)
        self.assertEqual(header["vocabulary_schema"], vp.VOCABULARY_SCHEMA_ENDINGS)
        self.assertEqual([(e["numeric_id"], e["key"]) for e in entries][-1],
                         (0xC002, "word.t1.end"))
        self.assertEqual(entries[-1]["category"], "ending")

    def test_ending_ids(self):
        self.assertEqual(vp.ending_id(1), 0xC001)
        self.assertEqual(vp.ending_id(0x3FFF), 0xFFFF)
        self.assertIsNone(vp.ending_id(0))
        self.assertIsNone(vp.ending_id(0x4000))

    def test_an_ending_without_its_word_or_in_schema_1_is_refused(self):
        with self.assertRaisesRegex(vp.FormatError, "has no word"):
            vp.build_payload(self.clips(word_id=3), **PACK_ARGS)
        bad = self.clips()
        bad[-1]["numeric_id"] = 0xC000
        with self.assertRaisesRegex(vp.FormatError, "namespace"):
            vp.build_payload(bad, **PACK_ARGS)
        p = bytearray(vp.build_payload(self.clips(), **PACK_ARGS))
        struct.pack_into("<I", p, 16, vp.VOCABULARY_SCHEMA)
        with self.assertRaisesRegex(vp.FormatError, "category"):
            vp.parse_payload(bytes(p))
        struct.pack_into("<I", p, 16, 3)
        with self.assertRaisesRegex(vp.FormatError, "does not allow"):
            vp.parse_payload(bytes(p))

    def test_the_c_readers_frozen_schema_2_fixture_reads_the_same(self):
        text = FIXTURE_V2_H.read_text(encoding="utf-8")
        body = text[text.index("] = {"):]
        payload = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-f]{2})\b", body))
        header, entries = vp.parse_payload(payload)
        self.assertEqual((header["pack_revision"], header["vocabulary_schema"]), (2, 2))
        self.assertEqual(len(entries), 21)
        self.assertEqual([(e["numeric_id"], e["key"], e["valid_samples"]) for e in entries
                          if e["category"] == "ending"],
                         [(0xC001, "word.front.end", 500), (0xC06D, "word.door.end", 520)])


class TrackedVoiceTests(unittest.TestCase):
    """AUD-023: export, verify and pack the tracked voice without a model or an encoder."""

    def setUp(self):
        self.v = SyntheticVoice()
        self.addCleanup(self.v.close)
        self.assertEqual(self.v.export(), [])

    def test_export_writes_a_voice_that_verifies(self):
        problems, report = self.v.verify()
        self.assertEqual(problems, [])
        self.assertTrue(report["draft"])
        self.assertEqual(report["pending"], len(self.v.rows))
        meta = self.v.meta()
        self.assertEqual(meta["pack_revisions"],
                         [{"revision": 1, "payload_sha256": report["payload_sha256"]}])
        front = meta["clips"][0]
        self.assertEqual(front["valid_samples"], self.v.lengths["word.front"])
        self.assertEqual(front["pre_skip"], 104)
        self.assertEqual(front["listening_review"], "pending")
        self.assertEqual(sorted(p.name for p in self.v.dst.iterdir()),
                         ["NOTICE.md", "masters", "packets", "provenance.json", "reference.wav",
                          "voice.json"])
        self.assertEqual((self.v.dst / "masters" / "word.front.wav").read_bytes(),
                         (self.v.src / "word.front.wav").read_bytes())

    def test_pack_twice_gives_the_same_bytes_and_a_draft_needs_allow_draft(self):
        out = Path(self.v.tmp.name) / "a.rvoc"
        refused = tracked.pack(self.v.dst, self.v.rows, self.v.lib, out)
        self.assertTrue(any("DRAFT" in p for p in refused), refused)
        self.assertFalse(out.exists())
        self.assertEqual(tracked.pack(self.v.dst, self.v.rows, self.v.lib, out,
                                      allow_draft=True), [])
        first = out.read_bytes()
        self.assertEqual(tracked.pack(self.v.dst, self.v.rows, self.v.lib, out,
                                      allow_draft=True), [])
        self.assertEqual(out.read_bytes(), first)
        self.assertEqual(hashlib.sha256(first).hexdigest(),
                         self.v.meta()["pack_revisions"][0]["payload_sha256"])
        envelope = i18n.unsigned_pack_file(first)
        self.assertEqual(tracked.check_built_pack(self.v.dst, self.v.rows, self.v.lib, envelope,
                                                  signed=False), [])
        other = bytearray(envelope)
        other[200] ^= 1
        self.assertTrue(tracked.check_built_pack(self.v.dst, self.v.rows, self.v.lib,
                                                 bytes(other), signed=False))

    def test_a_tampered_master_is_caught(self):
        f = self.v.dst / "masters" / "status.armed.wav"
        data = bytearray(f.read_bytes())
        data[-2] ^= 1
        f.write_bytes(bytes(data))
        problems, _ = self.v.verify()
        self.assertIn("masters/status.armed.wav: SHA-256 differs from voice.json", problems)
        out = Path(self.v.tmp.name) / "a.rvoc"
        self.assertTrue(tracked.pack(self.v.dst, self.v.rows, self.v.lib, out, allow_draft=True))

    def test_a_tampered_or_cut_packet_file_is_caught(self):
        f = self.v.dst / "packets" / "word.door.opuspkt"
        good = f.read_bytes()
        f.write_bytes(good[:3] + bytes([good[3] ^ 1]) + good[4:])
        problems, _ = self.v.verify()
        self.assertIn("packets/word.door.opuspkt: SHA-256 differs from voice.json", problems)
        f.write_bytes(good[:-1])
        problems, _ = self.v.verify()
        self.assertTrue(any("cut off" in p for p in problems), problems)

    def test_missing_and_extra_files_are_caught(self):
        (self.v.dst / "masters" / "walk.ended.wav").unlink()
        (self.v.dst / "packets" / "stray.opuspkt").write_bytes(b"x")
        problems, _ = self.v.verify()
        self.assertTrue(any("masters/: missing ['walk.ended.wav']" in p for p in problems))
        self.assertTrue(any("extra ['stray.opuspkt']" in p for p in problems))

    def test_a_script_that_moved_on_is_caught(self):
        rows = [dict(r) for r in self.v.rows]
        rows[0]["text"] = "Fore"
        problems, _ = tracked.verify(self.v.dst, rows, self.v.lib)
        self.assertTrue(any("word.front: text" in p for p in problems), problems)

    def test_a_revision_whose_payload_changed_is_refused(self):
        meta = self.v.meta()
        for c in meta["clips"]:
            c["listening_review"] = "accepted"      # no longer a DRAFT: other payload bytes
        self.v.save_meta(meta)
        problems, report = self.v.verify()
        self.assertFalse(report["draft"])
        self.assertTrue(any("not the one recorded for pack revision 1" in p for p in problems))
        out = Path(self.v.tmp.name) / "a.rvoc"
        self.assertTrue(tracked.pack(self.v.dst, self.v.rows, self.v.lib, out))
        self.assertFalse(out.exists())
        # Raising the revision and exporting records it, keeping revision 1's hash.
        self.v.lib["pack_revision"] = 2
        problems, _ = self.v.verify()
        self.assertTrue(any("revision 2 has no recorded payload" in p for p in problems))
        self.assertEqual(self.v.export(), [])
        revs = self.v.meta()["pack_revisions"]
        self.assertEqual([r["revision"] for r in revs], [1, 2])
        self.assertEqual(self.v.verify()[0], [])
        self.assertEqual(tracked.pack(self.v.dst, self.v.rows, self.v.lib, out), [])

    def test_export_keeps_a_review_only_while_the_take_is_unchanged(self):
        meta = self.v.meta()
        for c in meta["clips"]:
            c["listening_review"] = "accepted"
        self.v.save_meta(meta)
        self.v.lib["pack_revision"] = 2
        write_wav(self.v.src / "word.door.wav", 2430, amp=900)      # a new take, same length
        self.assertEqual(self.v.export(), [])
        reviews = {c["clip_id"]: c["listening_review"] for c in self.v.meta()["clips"]}
        self.assertEqual(reviews["word.door"], "pending")
        self.assertEqual(reviews["word.front"], "accepted")

    def test_export_refuses_a_stale_recording_and_writes_nothing(self):
        shutil.rmtree(self.v.dst)
        rows = [dict(r) for r in self.v.rows]
        rows[0]["spoken"] = "Fore."
        problems = tracked.export(self.v.src, self.v.dst, rows, self.v.lib, self.v.lengths)
        self.assertTrue(any("record it again" in p for p in problems), problems)
        self.assertFalse(self.v.dst.exists())
        lengths = dict(self.v.lengths, **{"word.front": self.v.lengths["word.front"] + 1})
        problems = tracked.export(self.v.src, self.v.dst, self.v.rows, self.v.lib, lengths)
        self.assertTrue(any("does not match" in p for p in problems), problems)
        self.assertFalse(self.v.dst.exists())

    def test_a_recorded_cut_is_kept_and_checked(self):
        meta = self.v.meta()
        meta["clips"][0]["cut"] = [100, 2500]
        meta["clips"][1]["cut"] = [2500, 100]
        self.v.save_meta(meta)
        problems, _ = self.v.verify()
        self.assertEqual([p for p in problems if "cut" in p],
                         [f"{meta['clips'][1]['clip_id']}: cut [2500, 100] is not "
                          "[first sample, end sample]"])

    def test_an_utterance_over_ten_seconds_is_caught(self):
        meta = self.v.meta()
        for c in meta["clips"]:
            if c["clip_id"] in ("status.armed", "status.night", "status.no_entry_delay"):
                c["valid_samples"] = 56000
        self.v.save_meta(meta)
        problems, _ = self.v.verify()
        self.assertTrue(any("status.armed + status.night + status.no_entry_delay" in p
                            for p in problems), problems)


@unittest.skipUnless((REAL_VOICE / "voice.json").exists(), "the tracked voice is not exported")
class TrackedEnUsTests(unittest.TestCase):
    """AUD-023: a clean checkout's en-US voice verifies and packs to the golden payload."""

    def test_verify_and_pack_twice_give_the_golden_payload(self):
        rows, problems = tracked.read_registry(SCRIPT, LIB)
        self.assertEqual(problems, [])
        problems, report = tracked.verify(REAL_VOICE, rows, LIB)
        self.assertEqual(problems, [])
        self.assertLessEqual(report["signed_size"], vp.FILE_MAX)
        meta = tracked.load_meta(REAL_VOICE)
        rows = tracked.voice_rows(meta, rows)
        first = tracked.payload_from(REAL_VOICE, meta, LIB)
        self.assertEqual(tracked.payload_from(REAL_VOICE, meta, LIB), first)
        self.assertEqual(LIB["pack_revision"], GOLDEN_REVISION, "a new revision: update the golden payload")
        self.assertEqual(hashlib.sha256(first).hexdigest(), GOLDEN_PAYLOAD_SHA256)
        header, entries = vp.parse_payload(first)
        self.assertEqual(len(entries), len(rows))
        self.assertEqual(header["flags"] & vp.FLAG_DRAFT,
                         vp.FLAG_DRAFT if tracked.is_draft(meta) else 0)


if __name__ == "__main__":
    unittest.main()
