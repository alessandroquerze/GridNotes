# GridNotes (C++)

Riscrittura completa della codebase in **C++ Win32 puro**, usando controlli **EDIT** standard (non RichEdit).

## Build

Su Windows (Developer Command Prompt):

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build
```

Eseguibile prodotto: `build/GridNotes.exe`.

## Note

- Salvataggio stato in `%APPDATA%\\GridNotes\\state.json`
- Persistenza in JSON di: cell size, dimensioni finestra, stato avvio automatico, lista tile
- Tile editabili con `EDIT` multilinea
- Menu contestuale su tile: split 2 orizzontale/verticale, split 4, elimina tile
- Toggle `Edit layout` per ridimensionamento dai bordi
- Toggle `Start with Windows` che aggiorna il registro (`HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run`)
