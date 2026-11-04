# REVELATION 1.0.0

**THE TRINITYKNOBS COLLECTION**  
By **TRIPLE SEVEN AUDIO**  
Mastering finisher

## What's in the download

```
REVELATION-1.0.0/
├── INSTALL.md            ← this guide
├── macOS/
│   ├── VST3/REVELATION.vst3
│   └── AU/REVELATION.component
└── Windows/
    └── REVELATION.vst3
```

Extract the ZIP first, then follow the section for your computer.

## macOS (Apple Silicon and Intel)

The `macOS` folder contains universal VST3 and Audio Unit plugins. They run natively on Apple Silicon and Intel Macs with macOS 11 or later.

1. Quit your DAW and extract the ZIP.
2. In Finder, choose **Go → Go to Folder** (Shift–Command–G) and open the folder for your format. If a folder is missing, create it under `~/Library/Audio/Plug-Ins`.

   | Format | Copy this entire item | To this folder |
   | --- | --- | --- |
   | VST3 | `macOS/VST3/REVELATION.vst3` | `~/Library/Audio/Plug-Ins/VST3/` |
   | Audio Unit | `macOS/AU/REVELATION.component` | `~/Library/Audio/Plug-Ins/Components/` |

3. Reopen your DAW and rescan. In **Ableton Live**, turn on VST3 System Folders or Audio Units in **Settings → Plug-Ins**, then click **Rescan**. In **Logic Pro**, use the Audio Unit. If it doesn't show up, open **Logic Pro → Settings → Plug-in Manager**, select REVELATION and click **Reset & Rescan Selection**.
4. Find **REVELATION** under **TRIPLE SEVEN AUDIO** and put it last on your master bus.

**If macOS blocks the plugin:** it isn't notarized by Apple yet. Open **System Settings → Privacy & Security** and click **Open Anyway**, or quit your DAW and run the line for the format you installed in Terminal:

```sh
xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/VST3/REVELATION.vst3"
xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/Components/REVELATION.component"
```

## Windows x64

1. Quit your DAW. Copy the entire `Windows/REVELATION.vst3` folder into `C:\Program Files\Common Files\VST3\`. Windows may ask for administrator permission.
2. Reopen your 64-bit VST3 host and rescan. In Ableton Live, turn on **VST3 System Folders** in **Settings → Plug-Ins**, then click **Rescan**.
3. Put **REVELATION** last on your master bus.

**If Windows blocks the ZIP:** before extracting, right-click it, choose **Properties**, tick **Unblock** and click **OK**.

## Controls

- **WEIGHT** adds low-end weight: a low shelf of up to +4.5 dB around 100 Hz, a gentle top-end tilt down, and sub harmonics on the mono low end so bass translates on small speakers.
- **SHEEN** adds top-end polish: an air shelf of up to +4.5 dB around 10 kHz plus subtle upper-harmonic sparkle.
- **LOUD** drives the mix up to +10 dB into a soft clipper and a lookahead peak limiter with a −1 dBFS ceiling. The **GR** readout under the knob shows how much the limiter is working. At 0% the limiter is off.
- **MIX** (the slider under the preset bar) blends the processed signal with the dry input. 100% is fully wet. Double-click it to return to 100%.
- **MATCH** turns the processed result down to your input's loudness, so you can judge the tone without being fooled by "louder sounds better". Turn it **off** before you bounce, or your master won't get louder.
- Presets: **Init**, **Streaming Ready**, **Club Weight**, **Airy Polish**, **Loud & Proud**. The disk icon saves a `.t7apreset` file and **••• → Open preset file** loads one. **••• → Bypass** toggles the plugin's bypass. Your DAW project recalls every control.

REVELATION adds about 1.5 ms of latency for its limiter lookahead and reports it to your DAW, which compensates automatically. At Init (all knobs at 0%) it passes audio through unchanged.

To uninstall, quit your DAW, delete the plugin you copied and rescan.
