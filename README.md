# Death Freedom Camera

A SKSE plugin for **Skyrim Special Edition 1.5.x** and **Skyrim Anniversary Edition 1.6.x** that allows free-camera movement after the player dies, by intercepting the `tfc` (Toggle Free Camera) console command in the death state.

A single FLATRIM binary is produced at build time and is loaded by both SE and AE at runtime.

---

## What it does

By default Skyrim silently refuses `tfc` once the player is dead, leaving you stuck on the bleedout camera. This plugin hooks the `tfc` console command and `TESCamera::SetState`, so that:

- Typing `tfc` while dead forces the camera into the free-camera state and keeps it there.
- Typing `tfc` again restores the bleedout camera.
- Player resurrection automatically clears the override.

Custom WASD translation and mouse-look are layered on top of the free-camera state, because the death UI swallows the normal `PlayerInputHandler` / `ProcessMouseMove` event flow.

---

## Installation

1. Make sure **SKSE64** is installed for your Skyrim version.
2. Drop `Death_Freedom_Camera.dll` (and its `.pdb` if you want symbols) into:
   ```
   <Skyrim folder>/SKSE/Plugins/
   ```
3. Confirm the plugin is listed in `SKSE/plugins.log` after launching the game. You should see lines similar to:
   ```
   Death_Freedom_Camera v1.0.0.0 loaded
   tfc console command hooked
   FreeCameraMover installed (tfc override + Bleedout camera override + SetState guard)
   ```

No INI, MCM, ESP or other configuration file is required.

---

## Usage

### 1. Activate free camera while dead

1. Die (or get into bleedout state).
2. Open the console with `~`.
3. Type:
   ```
   tfc
   ```
4. The camera switches to free-camera mode. Close the console to see the world.

### 2. Deactivate

Type `tfc` again in the console. The camera returns to the bleedout camera. You can also just resurrect — the override is automatically cleared.

> Note: while the death override is active, this plugin does **not** call the original `tfc` handler, so the vanilla time-freeze side-effect of `tfc` does not apply. Resurrect or type `tfc` again to release the override.

### 3. Controls

| Input | Action |
|---|---|
| `W` / `S` | Move forward / backward **along the camera's actual look direction** (follows pitch) |
| `A` / `D` | Strafe left / right (horizontal only) |
| `Shift` (hold) | Mouse-look — see below |
| Mouse | Move while `Shift` is held to rotate the view |
| `tfc` (console) | Toggle the death free-camera override on / off |

#### Mouse-look

- Press and hold `Shift` to enter mouse-look mode.
- On the first frame of holding `Shift`, the cursor is snapped to the screen center; the rotation is **not** snapped, so the view does not jump.
- While `Shift` stays held, the cursor is **not** pulled back each frame — you move the mouse freely and `dx/dy` is accumulated into the view rotation.
- When the cursor reaches the edge of the screen, release and re-press `Shift` to recenter and continue.
- Release `Shift` to stop mouse-look. The cursor position is preserved.

> Because we read the cursor through `GetCursorPos` (Windows screen coordinates) rather than raw mouse delta, the classic "infinite mouse" trick of re-centering the cursor every frame would feel like the cursor is being yanked away. The press-to-recenter model above avoids that.

### 4. Pitch range

Pitch is **not** clamped to a small angle. You can tilt the view freely and look all the way up / down.

A small caveat inherited from how `FreeCameraState` stores rotation as Euler angles (`pitch`, `yaw`):

- Around ±90° pitch the horizontal yaw component becomes numerically weak (`cos(pitch) → 0`), so horizontal mouse-look feels muted there.
- Pushing past ±90° causes the view to "flip" (upside-down). This is a limitation of Euler-angle accumulation, not a bug. If you want a truly gimbal-lock-free implementation, ask the author.

---

## Compatibility

- **Engine**: Skyrim Special Edition 1.5.97+ and Skyrim Anniversary Edition 1.6.x (single FLATRIM binary, version picked at runtime).
- **Framework**: CommonLibSSE-NG.
- **SKSE**: 2.2.6+.
- **Other mods**: Should be compatible with anything that doesn't patch the same camera-state vtable slots (`TESCameraState::Update` vtable index 3 for `FreeCameraState` and `BleedoutCameraState`) or the `tfc` script command entry. It does **not** touch quest, ESP/ESL or papyrus data, so quest and gameplay mods are unaffected.

---

## Building from source

### Requirements

- **Visual Studio 2022** (with the "Desktop development with C++" workload).
- **CMake ≥ 3.21**.
- **PowerShell** (the build script is a `.bat` that calls into vcvars64).
- **vcpkg** with `xbyak` (handled by the root `CMakePresets.json`).
- A working `CommonLibSSE-NG` checkout wired through the root project (see the root `README.md` and `CMakePresets.json`).

### Configure & build

From the repository root:

```bat
:: Configure (FLATRIM = SE + AE in one binary)
cmake -B Build -S . --preset=FLATRIM -Wno-dev

:: Build the plugin
msbuild "Build/Plugins/Death_Freedom_Camera/Death_Freedom_Camera.vcxproj" ^
        /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

Or simply run the helper at the repo root:

```bat
_build.bat
```

The output binary is written to `Build/bin/Release/Death_Freedom_Camera.dll`.

---

## Hook summary (for plugin authors / reviewers)

| Hook | Target | Purpose |
|---|---|---|
| `tfc` console command (`SCRIPT_FUNCTION::executeFunction`) | `tfc` | Toggle the death free-camera override; bypass vanilla tfc while dead |
| `FreeCameraState::Update` vfunc 3 | `cameraStates[kFree]` | Apply WASD translation and Shift+mouse rotation; force `a_nextState = Free` while the override is active |
| `BleedoutCameraState::Update` vfunc 3 | `cameraStates[kBleedout]` | Force `a_nextState = Free` while the override is active (fallback) |
| `TESCamera::SetState` (trampoline, 5-byte CALL) | `TESCamera` | Reject any attempt to switch into `BleedoutCameraState` while the override is active; clear the override on resurrection |

A global flag `g_deathTfcActive` is the single source of truth for the override state and is set / cleared only by the tfc hook and the `SetState` resurrection path.

---

## License

See the repository root `LICENSE`.
