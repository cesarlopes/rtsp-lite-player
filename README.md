# RTSP Lite Player

Lightweight Windows C++ player for viewing multiple RTSP camera streams in a single native mosaic window.

The project was built and tested with Tapo cameras, but it should work with other RTSP cameras that expose H.264/H.265 streams supported by FFmpeg.

## Features

- Native Windows mosaic window
- Multiple cameras from a JSON config file
- Parallel RTSP connection/decoding per camera
- Low-latency `fast_open` mode
- Video-only playback; audio streams are ignored
- FFmpeg-based RTSP and decoding
- Low CPU usage for small camera counts

## Formato de configuracao

Use JSON. It is safer than CSV/TXT for this use case because URLs and passwords can contain special characters, and it scales cleanly to multiple cameras.

Copie `config.example.json` para `config.json` e ajuste os dados.

Example:

```json
{
  "cameras": [
    {
      "name": "Front Door",
      "host": "192.168.0.50",
      "port": 554,
      "username": "camera_user",
      "password": "camera_password",
      "path": "/stream2",
      "rtsp_transport": "tcp",
      "fast_open": true
    }
  ]
}
```

You can also provide a full RTSP URL:

```json
{
  "name": "Front Door",
  "input_url": "rtsp://camera_user:camera_password@192.168.0.50:554/stream2",
  "fast_open": true
}
```

Do not commit `config.json`; it may contain private IPs and camera credentials.

## Build on Windows

Requirements:

- Visual Studio 2022 Build Tools with the C++ workload
- CMake
- vcpkg

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=D:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

The project includes `vcpkg.json`, so vcpkg installs `ffmpeg` and `nlohmann-json` automatically during CMake configuration.

The executable is generated as:

```text
build\Release\RtspLitePlayer.exe
```

If DLLs are missing when running the executable, copy the vcpkg runtime DLLs:

```powershell
copy build\vcpkg_installed\x64-windows\bin\*.dll build\Release\
```

## Run

```powershell
copy config.example.json config.json
notepad config.json
.\build\Release\RtspLitePlayer.exe config.json
```

If no argument is provided, the program tries to open `config.json` from the current working directory.

## Low-Latency Options

Optional fields per camera:

```json
"open_timeout_ms": 2000,
"read_timeout_ms": 3000,
"analyze_duration_us": 0,
"probe_size": 16384,
"max_delay_us": 200000,
"fast_open": true
```

Smaller values may reduce startup/failure wait time, but can make streams less tolerant of poor networks.

`fast_open` skips part of FFmpeg's initial stream analysis and requests video-only media. If video cannot be found, the app falls back to the normal path.

## Notes

- `/stream1` is commonly the high-quality stream on Tapo cameras.
- `/stream2` is commonly the lower-quality stream and is usually better for mosaics.
- The app currently renders with Win32/GDI. For many high-resolution cameras, a future Direct3D renderer may be more efficient.
