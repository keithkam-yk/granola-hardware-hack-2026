# Pigeon controller art

Pixel-art assets for the ESP32-S3 AMOLED controller. The `source/` directory
keeps the full-resolution generated masters; `runtime/` contains the compact
files intended for firmware integration.

| Runtime asset | Size | Role |
| --- | ---: | --- |
| `controller_screen_448x368.png` | 448x368 | Complete baseline controller screen |
| `pigeon_aviator.png` | 190x216 | Transparent idle character sprite |
| `flap_wings.png` | 84x67 | Transparent FLAP action icon |
| `deuce_payload.png` | 56x84 | Transparent DEUCE action icon |
| `flap_wings_animation_v1.gif` | 84x67 | Experimental four-frame FLAP loop |
| `deuce_payload_animation_v1.gif` | 56x84 | Experimental four-frame DEUCE loop |

The separate sprites let the firmware animate or highlight either action
without replacing the full-screen background. Preserve hard pixel edges when
resizing: use nearest-neighbour sampling and never upscale a runtime asset.

The versioned GIFs are non-destructive animation experiments. Their generated
four-frame source sheets live in `source/`, and `tools/build_action_gifs.py`
rebuilds the runtime files with a shared palette and transparent disposal.

## Art direction

- Cozy 16-bit pixel art with a midnight-navy cockpit backdrop.
- Slate-blue pigeon, cream feathers, brass/copper hardware, muted teal lenses.
- Aviator goggles, leather cap, and one small gear detail provide the light
  steampunk character.
- `FLAP` is energetic and readable; `DEUCE` is cheeky without gross detail.

Generated with the built-in image generator. The character prompt requested a
transparent, full-body aviator pigeon sprite. The action prompts used that
character as their palette/style reference and requested transparent wing and
falling-payload icons. The screen prompt composed those three assets into an
exactly labelled `PIGEON FLIGHT` / `FLAP` / `DEUCE` controller layout.
