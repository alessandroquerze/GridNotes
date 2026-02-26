progetto con win32 + EDIT (no richedit). creato in 1 giorno tramite AI, sia la versione in .NET che la successiva versione in C++ costruita da una riscrittura automatica del codice



Stato salvato in `%APPDATA%\\GridNotes\\state.json`


compile lines:

se attivato task scheduler(sconsigliato): 
g++ -std=c++20 -municode -mwindows -O2 -o GridNotes.exe src/main.cpp src/startup.cpp -ladvapi32 -lshell32 -lcomctl32 -lgdi32 -luser32 -lole32 -loleaut32 -luuid

se con registro run:

g++ -std=c++20 -municode -mwindows -O2 -o GridNotes.exe src/main.cpp -ladvapi32 -lshell32 -lcomctl32 -lgdi32 -luser32


esiste la versione iniziale creata in .NET con c#, non indicata perché molto piu lenta
