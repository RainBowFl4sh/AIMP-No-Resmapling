# AIMP AutoRate

AIMP-Plugin (64-bit, Windows 10/11), das beim Trackwechsel die **Abtastrate des Windows-Ausgabegeräts automatisch an die Datei anpasst**, damit nichts resampelt wird. Optional zieht es die komplette **Voicemeeter-Kette** (Standard, Banana, Potato) mit.

> **Status:** Gegen das AIMP SDK v6.00 (Build 3083) geschrieben, aber noch nicht praktisch getestet. Bitte mit eigener Hardware ausprobieren und die Logdatei prüfen.

## Was es macht

1. Bei jedem Track-Start liest das Plugin die Abtastrate der Datei (z. B. 44100, 48000, 96000 Hz).
2. Es ermittelt die Signalkette:
   - **Echte Hardware** (Realtek, DAC wie FiiO KA11): nur dieses Gerät wird behandelt, Voicemeeter wird ignoriert.
   - **Voicemeeter-Gerät** (VAIO, AUX, VAIO3): Voicemeeter wird automatisch erkannt (Standard/Banana/Potato). Die Geräte an A1 bis A3 bzw. A5 werden per Remote API ausgelesen und mit den Windows-Geräten abgeglichen.
   - **Kein Voicemeeter installiert oder gestartet:** das Voicemeeter-Modul bleibt komplett inaktiv.
3. Es wählt eine Rate, die **alle** Geräte der Kette unterstützen. Wird die Rate nicht von allen unterstützt, nimmt es ein ganzzahliges Vielfaches derselben Familie (44,1 → 88,2 → 176,4 kHz bzw. 48 → 96 → 192 kHz).
4. Stimmt alles schon, passiert **nichts** (keine Aussetzer bei Alben mit gleicher Rate).
5. Sonst: Wiedergabe stoppen, Windows-Geräteformat per `IPolicyConfig` setzen (zuerst Hardware, zuletzt das Gerät, in das AIMP spielt), optional Voicemeeter-Engine neu starten, kurz warten, Track neu starten.

## Voraussetzungen

- Windows 10/11, 64-bit, AIMP 64-bit
- [Visual Studio 2022](https://visualstudio.microsoft.com/) mit Workload "Desktopentwicklung mit C++"
- [CMake](https://cmake.org/download/)
- AIMP SDK (von [aimp.ru](https://www.aimp.ru), nicht im Repo enthalten)

## Bauen

SDK entpacken. Der benötigte Ordner ist `Sources\Cpp` (darin liegt `apiPlugin.h`).

```bat
build.bat "C:\Pfad\zum\aimp_sdk\Sources\Cpp"
```

Das Ergebnis liegt unter `build\Release\AIMP_AutoRate.dll`.

Direkt installieren (AIMP vorher beenden, Skript als Administrator starten):

```bat
build.bat "C:\Pfad\zum\aimp_sdk\Sources\Cpp" install
```

Manuell geht es auch: DLL nach `C:\Program Files\AIMP\Plugins\AutoRate\` kopieren.

## Aktivieren

AIMP starten, unter *Einstellungen → Plugins* "AutoRate" aktivieren.

## Konfiguration

Datei: `%APPDATA%\AIMP_AutoRate\config.ini` (wird beim ersten Start angelegt).

```ini
[General]
Enabled=1
UseVoicemeeter=1
RestartVoicemeeter=1
SettleMs=800

[Devices]
AimpDevice=
ExtraDevices=
```

| Einstellung | Bedeutung |
|---|---|
| `Enabled` | Plugin an/aus |
| `UseVoicemeeter` | Voicemeeter-Kette automatisch auflösen |
| `RestartVoicemeeter` | Engine nach dem Formatwechsel neu starten |
| `SettleMs` | Wartezeit nach dem Umschalten in Millisekunden |
| `AimpDevice` | Namensteil des Geräts, in das AIMP spielt. Leer = Windows-Standardgerät |
| `ExtraDevices` | Weitere Geräte zum Mitschalten, Namensteile getrennt durch `;` |

Welche Ausgabe AIMP nutzt, steht im Log (Zeile "AIMP-Ausgabe: ...").

## Log

`%APPDATA%\AIMP_AutoRate\autorate.log` zeigt erkannte Geräte, gewählte Rate und alle Umschaltvorgänge.

## Bekannte Einschränkungen

- Das Windows-Geräteformat lässt sich nur über die **undokumentierte** Schnittstelle `IPolicyConfig` ändern. Windows-Updates können das brechen.
- Bei einem Ratenwechsel wird der Track **kurz neu gestartet**, weil AIMP das Gerät öffnet, bevor das Plugin die Rate kennt.
- Voicemeeter arbeitet intern mit Float32. Ob die Engine nach dem Formatwechsel wirklich nicht resampelt, hängt vom Treibertyp der Ausgänge ab (WDM, KS, ASIO) und muss getestet werden. ASIO-Ausgänge in Voicemeeter lassen sich nicht über Windows-Formate steuern.
- Bei WASAPI Exclusive oder ASIO direkt auf den DAC ist das Plugin meist überflüssig, weil AIMP das Format dann selbst passend öffnet.
- Voicemeeter wird nie automatisch gestartet.

## Fehlersuche

- **Plugin erscheint nicht in AIMP:** DLL im richtigen Ordner? AIMP 64-bit verwendet?
- **Keine Umschaltung:** `autorate.log` prüfen. Evtl. `AimpDevice=` setzen.
- **Gerät unterstützt Rate nicht:** Log zeigt "Keine passende Rate". Der Treiber meldet im Exclusive-Modus keine Unterstützung.
- **Build-Fehler bei `Play2`, `GetInfo` usw.:** Prüfen, ob die SDK-Version v6.00 ist.

## Lizenz

Noch keine festgelegt. Das AIMP SDK unterliegt der Lizenz von AIMP und ist nicht Teil dieses Repos.
