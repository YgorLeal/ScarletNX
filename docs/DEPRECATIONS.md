<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Deprecations

What an older version used that this one replaced, how a run tells you, and how it goes away.

## The rule

- Something that changes meaning (a setting, an option, a path, a file's layout) gets a **new** name. The old name is
  never given a new meaning.
- The old one becomes **deprecated**. A deprecated setting is not read (the new one is), but it is kept where it is,
  never dropped in silence; a deprecated option still works.
- **Each run says so** where you read it: stderr for the drivers, the game's `.log` and stderr for the runtime, the
  launcher's box for files an older version left. The line names what replaced it.
- **You remove it**, by hand or with the launcher's **Settings > App > Clean old settings**. That takes the
  `[Deprecated]` section out of every game's `.ini` the launcher reaches (the games' config folder, `games/` and your
  game folders), and moves each file an older version left to its new place, deleting the old copy only when the new
  place already has one.
- **It is listed below** until the code that reads it goes, which is a change of its own named in the table.

A game's `.ini` keeps its deprecated keys last, under what became of each:

```ini
[Deprecated]
; keys an older version used, which do nothing now (docs/DEPRECATIONS.md): delete them, or use Settings > App >
;   Clean old settings in the launcher
; renderer: Vulkan is the only renderer
renderer=opengl
```

## Now

| Old | Replaced by | Now | How it is told |
|---|---|---|---|
| `.ini` keys `renderer`, `dynamic_resolution`, `dynamic_resolution_min`, `game_dynamic_resolution`, `low_priority_shader_compiler`, `smooth_recording`, `recording_fps`, `lsfg`, `lsfg_dll`, `lsfg_target`, `lsfg_multiplier`, `lsfg_flow`, `lsfg_resolution`, `lossless_scaling`, `lossless_path` | Remove the OpenGL renderer | nothing: they went with it | under `[Deprecated]`, named once a run |
| `.ini` key `audio_device` | [gpu/audio] Fix viewport flips and audio playback | playback follows the system's default | the same |
| `.ini` key `forced_vsync` | Remove the OpenGL renderer | `vsync` | the same |
| `.ini` keys `touch_controls`, `touch_controls_opacity` | [runtime] Isolate shared core and NX runtime | `controls`, `controls_opacity` | the same |
| `.ini` keys `modo_tv`, `idioma`, `resolucion`, `sombras`, `perfil` (SwitchRecomp's) | #185 | `docked`, `language`, `resolution`, `shadows`, `profile` | the same |
| `--keep-c` | Replace Python pipeline with a C90 driver | `--keep-ir` | still taken, said on stderr each time |
| An installed copy's files in `~/.local/share/scarletnx` on Linux: `gui/last-run.log`, `gui/recent.txt`, `games/<Game>.ini`, `games/<Game>.log`, `games/threads.txt`, `games/shader_cache/vk_pipelines.bin`, `work/` | #212 | the config, state and cache folders ([USAGE](USAGE.md#folders)) | each run names each one and where it goes now; the launcher shows them in a box as it opens |

A `.ini` key the runtime does not know at all (a newer version's, or a typo) is not deprecated: it stays under
`[Other]`, as it was, since a newer version may read it.
