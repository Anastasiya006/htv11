"""Audio tests use fake players: no speaker or API key needed."""
import base64
import io
import json
import os
import threading
import unittest
import wave
from unittest.mock import patch
import gemmi_audio as ga


class AudioTests(unittest.TestCase):
    def service(self, mode="chimes", **kwargs):
        self.played = []
        def player(path): self.played.append(ga.validate_wav(path.read_bytes()))
        service = ga.AudioService(mode, player=player, **kwargs)
        self.addCleanup(service.close)
        return service

    def wait(self, service):
        service.jobs.join()

    def test_all_chimes_are_valid_quiet_short_wav(self):
        for cue in ga.CUES:
            data = ga.validate_wav(ga.chime(cue))
            with wave.open(io.BytesIO(data)) as wav:
                self.assertEqual(wav.getframerate(), 24000)
                # Jingles (level-up fanfare) run a little over a second.
                self.assertLess(wav.getnframes() / 24000, 3)

    def test_routine_activity_is_silent(self):
        service = self.service()
        for kind, state in [("log", "success"), ("status", "thinking"), ("status", "working")]:
            self.assertFalse(service.submit(dict(kind=kind, state=state, agent="a", id=1)))
        self.assertEqual(service.status()["queued"], 0)

    def test_duplicate_event_and_success_spam_suppressed(self):
        service = self.service()
        event = dict(kind="status", state="success", agent="a", id=1)
        self.assertTrue(service.submit(event))
        self.assertFalse(service.submit(event))
        self.assertFalse(service.submit({**event, "id": 2}))
        self.wait(service)
        self.assertEqual(len(self.played), 1)

    def test_distinct_questions_are_not_suppressed(self):
        service = self.service()
        event = dict(kind="ask", state="needs-attention", agent="a", message="Proceed?")
        self.assertTrue(service.submit({**event, "id": 1}))
        self.assertTrue(service.submit({**event, "id": 2}))
        self.wait(service)
        self.assertEqual(len(self.played), 2)

    def test_speech_failure_falls_back_without_exposing_error(self):
        def broken(text): raise RuntimeError("secret response must not be logged")
        service = self.service("speech", synthesizer=broken)
        service.test("ready"); self.wait(service)
        self.assertEqual(len(self.played), 1)
        self.assertEqual(service.status()["last_error"], "Speech unavailable; used chime instead")

    def test_api_quota_failure_is_reported_without_response_body(self):
        def exhausted(text):
            raise ga.urllib.error.HTTPError("https://generativelanguage.googleapis.com",402,"quota",{},io.BytesIO(b"private-response"))
        service=self.service("speech",synthesizer=exhausted)
        service.test("ready"); self.wait(service)
        self.assertEqual(service.status()["last_error"],"Gemini HTTP 402; used chime instead")
        self.assertEqual(len(self.played),1)

    def test_speech_success_and_temporary_files_removed(self):
        paths=[]
        def player(path): paths.append(path); ga.validate_wav(path.read_bytes())
        service=ga.AudioService("speech", player=player, synthesizer=lambda text: ga.chime("ready"))
        self.addCleanup(service.close)
        service.test("ready"); self.wait(service)
        self.assertFalse(paths[0].exists())
        self.assertEqual(service.status()["played"], 1)

    def test_queue_is_bounded_and_questions_have_priority(self):
        blocked=threading.Event(); started=threading.Event(); order=[]
        def player(path):
            started.set(); blocked.wait(2); order.append(path.read_bytes())
        service=ga.AudioService("chimes", player=player)
        self.addCleanup(service.close)
        service.test("ready"); self.assertTrue(started.wait(1))
        for _ in range(7): self.assertTrue(service.test("success"))
        self.assertTrue(service.test("ask"))
        self.assertFalse(service.test("success"))
        self.assertEqual(service.status()["dropped"], 1)
        blocked.set(); self.wait(service)
        self.assertEqual(order[1], ga.chime("ask"))

    def test_answered_question_is_skipped_while_queued(self):
        blocked=threading.Event(); started=threading.Event(); played=[]
        def player(path):
            started.set(); blocked.wait(2); played.append(path.read_bytes())
        service=ga.AudioService("chimes", player=player)
        self.addCleanup(service.close)
        service.test("ready"); self.assertTrue(started.wait(1))
        event=dict(id=3, kind="ask", agent="a", message="Proceed?")
        self.assertTrue(service.submit(event))
        event["response"]={"choice":"Yes"}
        blocked.set(); self.wait(service)
        self.assertEqual(len(played),1)
        self.assertEqual(service.status()["dropped"],1)

    def test_invalid_test_sound_and_settings(self):
        service=self.service()
        for cue in ("invalid", [], None):
            with self.assertRaises(ValueError): service.test(cue)
        with self.assertRaises(ValueError): ga.AudioService("chimes", volume=2, player=lambda p: None)
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, "GEMINI_API_KEY"):
                ga.AudioService("speech", player=lambda p: None)

    def test_rest_speech_decodes_wav_and_keeps_key_out_of_url(self):
        payload={"candidates":[{"content":{"parts":[{"inlineData":{"data":base64.b64encode(ga.chime("ready")).decode(),"mimeType":"audio/wav"}}]}}]}
        class Response(io.BytesIO): pass
        def request(req, timeout):
            self.assertNotIn("test-key", req.full_url)
            self.assertEqual(req.get_header("X-goog-api-key"), "test-key")
            self.assertEqual(json.loads(req.data)["contents"][0]["parts"][0]["text"], "Hello")
            return Response(json.dumps(payload).encode())
        with patch.dict(os.environ, {"GEMINI_API_KEY":"test-key"}), patch.object(ga.urllib.request,"urlopen",side_effect=request):
            self.assertTrue(ga.gemini_speech("Hello").startswith(b"RIFF"))

    def test_speech_never_includes_agent_content(self):
        private=dict(agent="private-agent",message="private-command",artifact="private-path")
        for cue in ga.CUES:
            spoken=ga.speech_text(private,cue)
            self.assertNotIn("private",spoken)
            self.assertEqual(spoken,ga.speech_text({},cue))

    def test_dotenv_key_loading_and_environment_precedence(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as folder, patch.dict(os.environ, {}, clear=True):
            path=Path(folder)/".env"
            path.write_text('OTHER_VAR=never-load\nGEMINI_API_KEY="local-test-key" # note\n')
            self.assertTrue(ga.load_env_key(path))
            self.assertEqual(os.environ["GEMINI_API_KEY"],"local-test-key")
            self.assertNotIn("OTHER_VAR",os.environ)
            os.environ["GEMINI_API_KEY"]="existing-key"
            self.assertTrue(ga.load_env_key(path))
            self.assertEqual(os.environ["GEMINI_API_KEY"],"existing-key")

    @unittest.skipUnless(ga.importlib.util.find_spec("numpy"), "optional NumPy effect")
    def test_piano_voice_keeps_duration_and_safe_pcm(self):
        from gemmi_voice_effects import piano_speech
        import numpy as np
        original=ga.chime("ready")
        changed=ga.validate_wav(piano_speech(original))
        self.assertNotEqual(original,changed)
        with wave.open(io.BytesIO(original)) as a, wave.open(io.BytesIO(changed)) as b:
            self.assertEqual(a.getnframes(),b.getnframes())
            self.assertEqual(a.getframerate(),b.getframerate())
            samples=np.frombuffer(b.readframes(b.getnframes()),dtype="<i2")
            self.assertLessEqual(np.max(np.abs(samples.astype(float))),32767*0.35)

    @unittest.skipUnless(ga.importlib.util.find_spec("numpy"), "optional NumPy effect")
    def test_piano_voice_preserves_silence(self):
        from gemmi_voice_effects import piano_speech
        original=io.BytesIO()
        with wave.open(original,"wb") as wav:
            wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(24000)
            wav.writeframes(b"\0\0"*2400)
        changed=piano_speech(original.getvalue())
        with wave.open(io.BytesIO(changed)) as wav:
            self.assertEqual(wav.readframes(wav.getnframes()),b"\0\0"*2400)

    @unittest.skipUnless(ga.importlib.util.find_spec("numpy"), "optional NumPy effect")
    def test_chiptune_effect_is_distinct_and_preserves_duration(self):
        from gemmi_voice_effects import chiptune_speech, piano_speech
        original=ga.chime("ready")
        chip=ga.validate_wav(chiptune_speech(original))
        self.assertNotEqual(chip,piano_speech(original))
        with wave.open(io.BytesIO(chip)) as a, wave.open(io.BytesIO(original)) as b:
            self.assertEqual(a.getnframes(),b.getnframes())
            self.assertEqual(a.getframerate(),b.getframerate())

    def test_invalid_audio_rejected(self):
        with self.assertRaises((wave.Error, EOFError)): ga.validate_wav(b"invalid")
        out=io.BytesIO()
        with wave.open(out,"wb") as wav:
            wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(24000)
            wav.writeframes(b"\0\0"*24000*31)
        with self.assertRaises(ValueError): ga.validate_wav(out.getvalue())

if __name__ == "__main__": unittest.main()
