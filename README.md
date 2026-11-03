# XCAM VR Videos – Quest 3

Spielt VR180-3D-Videos der **SLAM XCAM** (und andere VR180/360°-Videos) in der Meta Quest 3 ab – als eigene App neben den Foto-Apps.

## So funktioniert es

1. App starten → **Anmelden** mit dem Google-Konto.
2. **Aus Google Fotos wählen** → Videos oder ein Album wählen → „Fertig“. Die Videos werden in den Ordner **Movies/XCAM** der Quest geladen – fortsetzbar und auch bei abgesetzter Brille (Quest am besten ans Ladekabel).
3. **Oder** Videos per USB (OpenMTP, Developer Hub) direkt in **Movies/XCAM** kopieren – das ist schneller und garantiert Originalqualität.
4. Video antippen → in 3D abspielen. Die App merkt sich pro Video die Stelle, an der du aufgehört hast, sowie Helligkeit, Weißpunkt und Darstellungsmodus.

**Hinweis:** Die Quest 3 spielt 6K mit 30 Bildern/s flüssig ab; 8K-Videos mit 60 Bildern/s schafft sie nicht. „Auto“ erkennt XCAM-Videos (zwei 180°-Bilder nebeneinander, 2:1) als VR180 3D. Wirkt das 3D „falsch herum“ (schielt), mit Y die Augen tauschen.

## Bedienung in VR

| Taste | Funktion |
|---|---|
| **Rechter Controller** | |
| A | Abspielen / Pause |
| Thumbstick links / rechts | 10 Sekunden zurück / vor (halten = weiter spulen) |
| Thumbstick oben / unten | heller / dunkler – drücken = 100 % |
| B | nächstes Video |
| Trigger (Zeigefinger) | zurück zur Übersicht |
| **Linker Controller** | |
| Thumbstick oben / unten | Lautstärke |
| Thumbstick links / rechts | Weißpunkt kühler / wärmer – drücken = neutral |
| Trigger | vorheriges Video |
| X | Darstellungsmodus (Auto → VR180 3D → VR180 2D → 360° 2D → 360° 3D → Flach 2D → Flach 3D) |
| Y | Augen tauschen (bei 3D) |
| ☰ (Menü) | zurück zur Übersicht |
| **Beide** | |
| Seitentaste (Griff) | Bildfilter: Supersampling → Schärfen → aus |
| Meta-Taste lang drücken | Blickrichtung neu zentrieren |

## Einrichtung

Wie bei den Foto-Apps: neues GitHub-Repository (z. B. `xcam-video`), Projekt hochladen, Secrets `GOOGLE_CLIENT_ID` und `GOOGLE_CLIENT_SECRET` mit denselben Werten anlegen, unter Actions bauen, mit Meta Quest Developer Hub installieren.

## Technik

Das Video wird mit Androids MediaPlayer (Hardware-Decoder) in eine OpenXR-Android-Surface-Swapchain (`XR_KHR_android_surface_swapchain`) dekodiert und von der Quest direkt als Equirect-Ebene gezeichnet – ohne Umweg über die App-Grafik.
