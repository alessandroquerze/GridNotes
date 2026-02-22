progetto con win32 + EDIT (no richedit)



Stato salvato in `%APPDATA%\\GridNotes\\state.json`


compile lines:

se attivato task scheduler(sconsigliato): 
g++ -std=c++20 -municode -mwindows -O2 -o GridNotes.exe src/main.cpp src/startup.cpp -ladvapi32 -lshell32 -lcomctl32 -lgdi32 -luser32 -lole32 -loleaut32 -luuid

se con registro run:

g++ -std=c++20 -municode -mwindows -O2 -o GridNotes.exe src/main.cpp -ladvapi32 -lshell32 -lcomctl32 -lgdi32 -luser32