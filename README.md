# winkill

 WinKill v4 - Emulazione xkill per Windows 10/11
   - Icona nel system tray (notification area)
  - Click sinistro sull'icona → attiva la modalità kill (overlay + mirino)
  - Click destro sull'icona → menu contestuale (Avvia / Esci)
  - Usa IUIAutomation per la taskbar Win11 + fallback toolbar Win10
 
  Compilazione MinGW (mingw-w64, 64-bit):
    g++ -o winkill.exe winkill.cpp -lgdi32 -luser32 -lole32 -luiautomationcore -mwindows -std=c++17
 
  Compilazione MSVC:
    cl winkill.cpp /std:c++17 /link user32.lib gdi32.lib ole32.lib uiautomationcore.lib shell32.lib /SUBSYSTEM:WINDOWS
 
