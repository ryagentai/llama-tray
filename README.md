# llama-tray

A lightweight, native Win32 system tray host and on-demand model manager for `llama.cpp` powered by `llama-swap`.

## Features

- 🚀 **100% Native Win32 C**: Zero console windows, zero `.bat` wrappers, pure standalone executable.
- ⚡ **Asynchronous Threading**: Strict decoupling of UI thread and background workers (0% UI freeze / instant click response).
- 📊 **Graphical Progress HUD**: Real-time floating progress bar card in the bottom-right corner displaying tensor weight loading percentage.
- 🧠 **Auto-Scanner & Dynamic Rule Engine**:
  - Automatically detects and hooks external MTP Drafter models (`*-assistant.gguf`).
  - Automatically attaches multimodal vision projectors (`mmproj-*.gguf`).
  - Automatically enables native MTP acceleration for supported architectures (e.g. Qwen3.8).
  - Automatically applies `q8_0` KV-cache quantization for large models (≥14B) to prevent VRAM overflow.
- 🔌 **Unified OpenAI API**: Exposes standard `http://127.0.0.1:8888/v1` endpoint with automatic on-demand loading and idle VRAM reclamation.

## Build

Compile with GCC (MinGW-W64):

```bash
gcc -O2 -DUNICODE -D_UNICODE -Wl,-subsystem,windows -o llama-tray.exe tray.c -lwinhttp -lshell32 -lgdi32 -luser32 -lcomctl32 -lkernel32 -lole32 -lcomdlg32
```
