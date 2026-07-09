# LibreSCC

LibreSCC is a MIDI player and real-time synthesizer inspired by Gashisoft GXSCC

It converts MIDI files into a chiptune-style performance using SCC-inspired synthesis, with support for live playback and audio export.

## Features

- Real-time MIDI playback
- SCC-inspired waveform synthesis
- GTK4 graphical interface
- WAV export
- MP3 export (when libmp3lame is available)
- MIDI instrument mapping
- Multi-channel playback

## Screenshots

<img width="692" height="525" alt="image" src="https://github.com/user-attachments/assets/187efc85-01e7-44d7-8c59-81f6593ae17c" />


## Building

### Dependencies

Arch Linux:

```bash
sudo pacman -S gtk4 portaudio lame cmake gcc pkgconf
```

Debian/Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config libgtk-4-dev portaudio19-dev libmp3lame-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake pkgconf-pkg-config gtk4-devel portaudio-devel lame-devel
```

### Installation

Manual (Arch Linux):

```bash
git clone https://github.com/polska4au/librescc.git
cd librescc
makepkg -si
```

Manual (Debian, Ubuntu, Fedora, whatever else.):

```bash
git clone https://github.com/polska4au/librescc.git
cd librescc
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/librescc
```


This app was made with the help of Claude.
