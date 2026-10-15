"""The local text-to-speech engines, one small adapter each (runs inside that engine's environment).

Every adapter has `prepare(voice, reference_text, folder)`, which readies one voice from
audition.json (for a designed voice it first speaks a reference once and then clones it, so
every clip has the same voice), and `say(text, seed)`, which returns (samples, rate) with
samples as floats in [-1, 1]. `models` maps each Hugging Face repository used to its revision.
"""
import gc


def hf_revision(repo):
    """The commit of the cached snapshot (downloading it first if needed)."""
    from pathlib import Path
    from huggingface_hub import snapshot_download
    return Path(snapshot_download(repo)).name


def _floats(a):
    import numpy as np
    if hasattr(a, 'detach'):
        a = a.detach().float().cpu().numpy()
    return np.asarray(a, dtype=np.float64).reshape(-1).tolist()


class Kokoro:
    REPO = 'hexgrad/Kokoro-82M'
    RATE = 24000

    def __init__(self):
        from kokoro import KPipeline
        self.pipeline = KPipeline(lang_code='a', repo_id=self.REPO)  # 'a': American English
        self.models = {self.REPO: hf_revision(self.REPO)}

    def prepare(self, voice, reference_text, folder):
        self.voice = voice['voice']
        self.speed = voice.get('speed', 1.0)

    def say(self, text, seed):
        import torch
        parts = [audio for _, _, audio in self.pipeline(text, voice=self.voice, speed=self.speed)
                 if audio is not None]
        return _floats(torch.cat(parts)), self.RATE


class Qwen3:
    CUSTOM = 'Qwen/Qwen3-TTS-12Hz-1.7B-CustomVoice'
    DESIGN = 'Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign'
    BASE = 'Qwen/Qwen3-TTS-12Hz-1.7B-Base'
    MAX_TOKENS = 240  # 12 tokens a second: 20 s, far beyond any clip here

    def __init__(self):
        self.repo = None
        self.model = None
        self.models = {}

    def _load(self, repo):
        if repo != self.repo:
            import torch
            from qwen_tts import Qwen3TTSModel
            self.model = None
            gc.collect()
            torch.cuda.empty_cache()
            self.model = Qwen3TTSModel.from_pretrained(repo, device_map='cuda:0',
                                                       dtype=torch.bfloat16)
            self.repo = repo
            self.models[repo] = hf_revision(repo)
        return self.model

    def prepare(self, voice, reference_text, folder):
        import torch
        from audio_edit import read_wav, write_wav
        self.voice = voice
        if voice['kind'] == 'preset':
            self._load(self.CUSTOM)
            return
        ref = folder / 'reference.wav'
        if not ref.exists():
            m = self._load(self.DESIGN)
            torch.manual_seed(voice.get('seed', 1))
            wavs, rate = m.generate_voice_design(text=reference_text, language='English',
                                                 instruct=voice['design'],
                                                 max_new_tokens=self.MAX_TOKENS)
            write_wav(ref, _floats(wavs[0]), rate)
        m = self._load(self.BASE)
        x, rate = read_wav(ref)
        import numpy as np
        self.prompt = m.create_voice_clone_prompt(ref_audio=(np.asarray(x, dtype=np.float32), rate),
                                                  ref_text=reference_text, x_vector_only_mode=False)

    def say(self, text, seed):
        import torch
        torch.manual_seed(seed)
        if self.voice['kind'] == 'preset':
            wavs, rate = self.model.generate_custom_voice(
                text=text, language='English', speaker=self.voice['speaker'],
                instruct=self.voice.get('instruct'), max_new_tokens=self.MAX_TOKENS)
        else:
            wavs, rate = self.model.generate_voice_clone(
                text=text, language='English', voice_clone_prompt=self.prompt,
                max_new_tokens=self.MAX_TOKENS)
        return _floats(wavs[0]), rate


class VoxCPM2:
    REPO = 'openbmb/VoxCPM2'

    def __init__(self):
        from voxcpm import VoxCPM
        self.model = VoxCPM.from_pretrained(self.REPO, load_denoiser=False)
        self.rate = self.model.tts_model.sample_rate
        self.models = {self.REPO: hf_revision(self.REPO)}

    def _gen(self, text, seed, **kw):
        import torch
        torch.manual_seed(seed)  # VoxCPM 2.0.3's generate() takes no seed of its own
        wav = self.model.generate(text=text, cfg_value=2.0, inference_timesteps=10, **kw)
        return _floats(wav), self.rate

    def prepare(self, voice, reference_text, folder):
        from audio_edit import write_wav
        ref = folder / 'reference.wav'
        if not ref.exists():
            x, rate = self._gen(f"({voice['design']}){reference_text}", voice.get('seed', 1))
            write_wav(ref, x, rate)
        self.ref = str(ref)
        self.ref_text = reference_text

    def say(self, text, seed):
        return self._gen(text, seed, reference_wav_path=self.ref, prompt_wav_path=self.ref,
                         prompt_text=self.ref_text)


ENGINES = {'kokoro': Kokoro, 'qwen3': Qwen3, 'voxcpm2': VoxCPM2}
