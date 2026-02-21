# GridNotes (C++ Win32)

Sì: CMake non è obbligatorio.

Il progetto usa Win32 API + controlli `EDIT` standard (non RichEdit) e può essere compilato direttamente con **g++ (MinGW-w64)** su Windows.

## Build senza CMake (consigliato)

Apri un terminale MinGW su Windows e lancia:

```bash
g++ -std=c++20 -municode -mwindows -O2 -o GridNotes.exe src/main.cpp -ladvapi32 -lshell32 -lcomctl32 -lgdi32 -luser32
```

Questo produce `GridNotes.exe` nella cartella corrente.

## Note

- Stato salvato in `%APPDATA%\\GridNotes\\state.json`
- Persistenza JSON di impostazioni + tile
- Toggle `Start with Windows` con registry `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run`
- Tile con `EDIT` multilinea, split 2/4, delete, resize bordi in edit mode
