# RBR DS4 Rumble

Created by **porsche930rally / infiniteboost45**.

A lightweight Windows companion for **Richard Burns Rally / RallySimFans NGP**. It converts live suspension and motion telemetry into DualShock 4 vibration through the virtual Xbox controller created by DS4Windows.

## Setup

1. Open **DS4Windows**, connect the DualShock 4, and use a profile whose output controller is **Xbox 360**.
2. In the **RSF Launcher**, open **Advanced > Telemetry**, enable UDP telemetry, and set the destination to `127.0.0.1:6776`.
3. Run `RBR-DS4-Rumble.exe`. Press **TEST HEAVY** and **TEST LIGHT** to confirm both motors.
4. Leave the app running and start a stage. The RBR indicator turns green when packets arrive.

The app is portable. Its settings are saved beside the executable in `RBR-DS4-Rumble.ini`.

## Feedback channels

- **Impacts**: large damper movement and vertical acceleration changes, mainly on the heavy motor.
- **Road texture**: average movement of all four dampers, mainly on the light motor.
- **Engine**: speed-aware RPM pulse on the light motor.
- **Lateral load**: sideways acceleration cue for slides and hard cornering.
- **Smoothing**: higher settings soften sharp changes; lower settings feel rawer.

If another telemetry program already owns port 6776, change the port in both programs. UDP listeners cannot normally share the same local port.

## Notes

- The app sends standard XInput vibration. DS4Windows relays that feedback to the physical DS4.
- “Auto” uses the first connected XInput controller. Select Controller 1–4 if another gamepad receives the rumble.
- A telemetry watchdog stops both motors within 350 ms if RBR stops sending data.
- Windows 10/11 x64. No installer or additional runtime is required.

## Build

The release executable is built from `src/main.cpp` with LLVM-MinGW:

```powershell
clang++ -std=c++20 -O2 -municode -mwindows -static -o RBR-DS4-Rumble.exe src/main.cpp -lws2_32 -lcomctl32 -lgdi32 -luser32
```

Telemetry layout follows the 664-byte NGP UDP structure (`rbr.telemetry.data.TelemetryData.h`), including RPM, motion acceleration, and individual damper piston velocities.

## v1.1 changes

- Immediate rumble attack; smoothing now affects release only.
- 1 ms worker scheduling and queued-packet draining for lower latency.
- Double-buffered, clipped UI painting with change-only text updates to remove flicker.

## v1.1.1 changes

- Dark-themed the existing sliders, controller selector, checkbox, and UDP field without changing their behavior or layout.

## v1.1.2 changes

- Removed the global forced-dark flag that could cover labels with dark blocks.
- Gave every label an explicit panel/background color so all text and percentages remain visible.

## v1.2 changes

- Added spatial left/right suspension mixing from all four RBR dampers.
- Right-side gutters and impacts primarily drive the right-grip motor; left-side events primarily drive the left-grip motor.
- Kept 12% crossfeed so directional effects remain strong despite the DS4's different motor types.

## v1.2.1 changes

- Added a saved **Mute vibration while airborne** checkbox, enabled by default.
- Detects sustained four-wheel suspension unload from all four spring-deflection channels.
- Silences both motors immediately in the air and restores spatial feedback on landing.
