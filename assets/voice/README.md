# Voices

The panel's spoken words, one folder per locale and voice: `<locale>/<engine>-<voice>/`. They
are made on a computer with local text-to-speech (ADR-052) and tracked here so that any clone
builds the identical voice pack without a model (AUD-023). `tools/voice/voice.py export` writes
them, `verify` checks them and `pack` builds the pack ([tools/voice/README.md](../../tools/voice/README.md)
"The tracked voice"). The one hand edit is the maintainers': `"listening_review": "accepted"` in
`voice.json` for a take they have heard and accept, then a new `pack_revision` and `export`.

| Voice | Clips | State |
|---|---|---|
| `en-US/qwen3-designed_female` | 237 | every take awaits the maintainers' listening review: the pack is a DRAFT |

Each voice has its `NOTICE.md`: the voices are synthetic, and shared under the repository's
licence.
