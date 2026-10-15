# h264_sps

**Tier:** core (portable C11, no heap, no ESP-IDF) · **Feature:** [31](../../../docs/features/31-Browser-Panel-Manager/README.md) "Remote keypad"

The remote keypad streams the panel's screen as H.264 from the P4's hardware encoder
(`esp_h264`). That encoder writes a sequence parameter set whose VUI carries the frame rate only.
Without `bitstream_restriction` a browser's decoder must allow for pictures arriving out of order
and may hold back a whole picture buffer before it shows the first one (Chromium's hardware
decoders: about seven pictures). `h264_sps_low_delay()` rewrites the SPS of every key picture
before it leaves the panel:

- everything up to the VUI unchanged (the slices depend on it);
- no reordering (`max_num_reorder_frames` 0) and one picture buffered;
- the colour of what the P4's PPA produces: BT.601, limited range;
- the encoder's timing kept.

Anything it doesn't understand (scaling matrices, HRD parameters, a damaged SPS) is left alone:
the caller sends the picture as it was, which still decodes, only later.

| File | What |
|---|---|
| `include/h264_sps/h264_sps.h` | `h264_sps_low_delay()`, `h264_has_idr()` |
| `src/h264_sps.c` | the SPS parser and writer, emulation prevention both ways |
| `test/test_h264_sps.c` | an SPS written exactly as esp_h264 1.4.1 writes it for 1024x600, rewritten and read back (WEB-020) |

Used by `firmware/crowpanel_p4/main/dev_adapter.c`.
