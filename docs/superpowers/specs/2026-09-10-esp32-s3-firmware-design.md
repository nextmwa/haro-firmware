# Haro firmware su ESP32-S3 — design

## Contesto

Il progetto Haro (assistente vocale personale) è oggi implementato in Python
su Raspberry Pi 3B+ (repo sorella `haro/`), collegato via I2S "raw"
(INMP441 + MAX98357A) e I2C (SSD1306) su breadboard, e parla con un backend
AI (`server/`, FastAPI/WebSocket) invariato.

Dopo una sessione di troubleshooting hardware sul Pi (sottotensione
cronica sull'alimentazione, un modulo OLED difettoso) si è deciso di
affiancare — non sostituire — quel lavoro con un nuovo dispositivo basato
su **Waveshare ESP32-S3-AUDIO-Board**, che integra in hardware quello che
sul Pi era stato assemblato a mano su breadboard: codec audio (ES8311 +
ES7210 + ampli **NS4150B**), espansore GPIO (TCA9555), RTC, gestione
batteria (il regolatore MP1605GTF-Z genera l'alimentazione ~3.3V della
board — non è l'amplificatore audio, correzione rispetto a una prima
lettura della scheda tecnica).

Questo documento descrive il design del nuovo firmware. Il codice Python
esistente in `haro/` **non viene toccato** e resta il riferimento
comportamentale (stessa macchina a stati, stesso protocollo) da cui questo
firmware viene derivato.

## Obiettivo

Riprodurre lo stesso comportamento end-to-end di `haro/` (wake word →
cattura → streaming al server → riproduzione risposta + faccina che
riflette lo stato/emozione) su ESP32-S3, usando il più possibile
componenti nativi/ufficiali del framework invece di codice scritto da
zero, per minimizzare la superficie di manutenzione.

## Fuori scope

- Modifiche al backend `server/` (protocollo invariato, vedi sotto)
- Modifiche al progetto `haro/` per Raspberry Pi (resta com'è)
- Wake word custom "Ehi Haro" (si parte con "Hi ESP", il default
  pre-addestrato di WakeNet; una wake word custom è un'iterazione futura,
  fuori da questa spec)

## Dove vive il codice

Nuova cartella, sorella di `haro/` e `server/`, repo git separato:

```
Haro/
├── haro/              ← invariato (client Raspberry Pi)
├── server/             ← invariato (backend AI)
└── esp32-firmware/     ← NUOVO — progetto ESP-IDF
```

## Requisiti di toolchain

- **ESP-IDF v6.1** (versione stabile corrente al momento di questa spec).
  Nessun supporto a v5.x: Xiaozhi (progetto di riferimento più maturo per
  questo tipo di hardware) ha droppato IDF 5.x per lo stesso motivo —
  ESP-SR e i componenti codec più recenti assumono le API v6.x.
- Target: `esp32s3`.
- **ESP-SR: usare `espressif/esp-sr` versione `>=2.5.0`** (idealmente
  l'ultima, `2.5.3` al momento di questa spec) — è la versione che ha
  risolto un'incompatibilità nota tra Kconfig di ESP-SR e IDF v6.1. Il
  demo ufficiale Waveshare per questa board fissa `^2.1.5` (una versione
  precedente al fix, nel suo manifest), ma il vincolo caret (`^2.1.5`)
  risolve comunque all'ultima `2.x` disponibile sul registro se non la si
  fissa esplicitamente più stretta — non declassare a `2.1.5` esatto.

## Hardware target

Waveshare ESP32-S3-AUDIO-Board:
- ESP32-S3R8 (dual-core, 8MB PSRAM, 16MB flash)
- Codec **ES8311** (playback) + **ES7210** (mic array a 4 canali, con
  cancellazione eco) + ampli **NS4150B** — bus I2C interno alla board,
  non condiviso con l'header esterno
- Espansore GPIO **TCA9555** (per pulsanti/LED onboard)
- Header 18 pin: 11 GPIO liberi (GPIO3-11, GPIO19-20) + 3 EXIO (via
  TCA9555, I2C, più lenti) + alimentazione (5V, 3V3, GND)
- Display **SSD1306 esterno** (riuso dell'hardware già nel progetto Pi),
  collegato su 2 dei GPIO liberi dell'header come bus I2C dedicato (es.
  GPIO8=SDA, GPIO9=SCL), separato dal bus I2C interno della board

**Pin mapping interno verificato** (dal firmware demo ufficiale
Waveshare, `ESP32-S3-AUDIO-Board-Demo.zip` → `ESP-IDF/esp_sr_02/main/hardeware_driver/bsp_board.h`
— non da fonti terze o dedotto dallo schema):

| Segnale | GPIO |
|---|---|
| I2C SCL (controllo ES8311/ES7210/TCA9555) | GPIO10 |
| I2C SDA | GPIO11 |
| I2S MCLK | GPIO12 |
| I2S BCLK | GPIO13 |
| I2S WS/LRCK | GPIO14 |
| I2S DIN (mic, ES7210→ESP32) | GPIO15 |
| I2S DOUT (speaker, ESP32→ES8311) | GPIO16 |

Stesso bus I2S condiviso (porta `I2S_NUM_1`) per capture e playback,
stesso principio già validato sul Pi con l'overlay `haro-duplex`, qui
però gestito nativamente da `esp_codec_dev` invece che da un device tree
overlay scritto a mano.

## Componenti (mappatura su moduli nativi ESP-IDF/Espressif)

| Component progetto | Componente nativo/ufficiale | Responsabilità nostra |
|---|---|---|
| `audio_pipeline` | `esp_codec_dev` (Espressif, `espressif/esp_codec_dev` sul registro) + `esp_io_expander_tca95xx_16bit` (Espressif, per il TCA9555) | Configurazione specifica della board (pin/indirizzi qui sopra), porting da `bsp_board.c` di Waveshare |
| `wake_word` | ESP-SR (`espressif/esp-sr`, AFE + WakeNet "hiesp" + VAD) | Collegamento tra AFE e orchestrator (code FreeRTOS), adattato da `mic_speech.c` di Waveshare (senza la parte MultiNet/comandi vocali, non ci serve) |
| `protocol` | cJSON (incluso in ESP-IDF) | Schema messaggi: `hello`, `end_of_speech`, `emotion`, `response_end`, `error` — identico a `haro/src/haro/protocol.py` |
| `server_client` | `esp_websocket_client` (componente managed ufficiale) | Riconnessione con backoff esponenziale, stessa logica di `server_client.py` |
| `face_display` | `esp_lcd` + `esp_lcd_panel_ssd1306.h` (**core ESP-IDF**, non componente esterno — vedi nota sotto) | Porting della logica di disegno "Cozmo-style" da `face_display.py` (occhi a pillola inclinabili, bocca ad arco con estremità arrotondate), framebuffer composto a mano e passato a `esp_lcd_panel_draw_bitmap` |
| `wifi_provisioning` | `network_provisioning` (componente managed ufficiale, **non** `wifi_provisioning` — rinominato in ESP-IDF v6.x, vedi nota sotto) | Configurazione (SSID hotspot, ecc.) |
| `config` | NVS (storage nativo ESP-IDF) | Schema chiavi (`server_url`, ecc.), sostituisce il file `config.json` del Pi |
| `orchestrator` | — (logica di business specifica di Haro) | Intera macchina a stati, porting diretto di `orchestrator.py` |

**Nota sul display** — il componente `espressif/ssd1306` del registro
(quello inizialmente previsto) è **deprecato** ("please use updated
SSD1306 driver from ESP-IDF"). Il sostituto è integrato nel core ESP-IDF
sotto l'astrazione `esp_lcd` (`esp_lcd_new_panel_ssd1306()`), da
aggiungere solo con un `#include`, senza dipendenza nel registro.
`esp_lcd` è un layer di trasporto (spinge un framebuffer, non ha
primitive di disegno) — la logica di rettangoli arrotondati/archi resta
comunque nostra, come previsto, cambia solo la funzione di push finale.

**Nota sul WiFi provisioning** — `wifi_provisioning` è stato rimosso dal
core ESP-IDF in v6.x, sostituito dal componente managed
`network_provisioning` (stessa funzione, prefisso API `network_prov_*`
invece di `wifi_prov_*`). **Punto aperto, non risolto dalla sola
documentazione**: non è confermato se lo schema SoftAP di
`network_provisioning` serva di suo una pagina web compilabile da
browser (come il captive portal custom del Pi oggi) o si aspetti il
protocollo protocomm/protobuf parlato dall'app companion Espressif o dal
tool `esp_prov.py`. Va verificato leggendo il sorgente dello schema
SoftAP prima di implementare quel task — se non offre una pagina HTML
pronta, serve decidere se accettare il flusso "app companion" (cambio di
UX reale rispetto a oggi) o costruire una pagina custom sopra gli
endpoint HTTP nativi.

Riferimento architetturale (non di protocollo): il progetto open-source
**Xiaozhi** (`78/xiaozhi-esp32`, con build mantenuta da Waveshare per
hardware simile) dimostra che questa combinazione (AFE+WakeNet+streaming
verso un backend cloud) funziona bene su ESP32-S3. Non ne adottiamo il
protocollo/server — `server/` resta invariato — ma il suo codice è un
riferimento utile per l'inizializzazione di `esp_codec_dev`/ESP-SR invece
di scrivere quella parte da zero.

## Protocollo verso il server (invariato)

Stesso schema di oggi, nessuna modifica lato `server/`:

- **Client → server** (testo JSON via `cJSON`): `hello` (session_id),
  `end_of_speech`
- **Client → server** (binario, frame WebSocket raw): frame audio PCM
  catturati dal mic
- **Server → client** (testo JSON): `emotion` (value), `response_end`,
  `error` (message)
- **Server → client** (binario): chunk audio TTS da riprodurre

## Macchina a stati (orchestrator)

Stessa di `orchestrator.py`, porting diretto:

```
IDLE --(wake word "Hi ESP")--> LISTENING
LISTENING --(fine parlato/VAD)--> THINKING
THINKING --(end_of_speech inviato)--> attesa eventi server
  evento "emotion" --> stato SPEAKING, faccina aggiornata
  evento audio binario --> riproduzione via esp_codec_dev
  evento "response_end" --> IDLE
  evento "error" / timeout / errore socket --> faccina ERROR, riconnessione, IDLE
```

## Concorrenza (FreeRTOS, pattern nativo)

- **Task audio/wake-word**: possiede la pipeline AFE di ESP-SR. Rileva
  wake word e fine-parlato, pubblica eventi su una coda FreeRTOS.
- **Task orchestrator**: consuma quella coda, pilota la macchina a stati,
  chiama `server_client` e `face_display`.
- **`esp_websocket_client`**: gira nel proprio task (pattern nativo del
  componente), consegna eventi via callback → tradotti con `protocol` e
  messi in coda per l'orchestrator.
- **Riproduzione**: scrittura diretta su `esp_codec_dev` quando arrivano
  chunk audio dal server.

## Display

Porting 1:1 della logica di `face_display.py` (rettangoli arrotondati
inclinabili per gli occhi, archi con estremità arrotondate per la bocca,
le 9 espressioni già definite: IDLE, LISTENING, THINKING,
SPEAKING_HAPPY/SAD/CONFUSED/NEUTRAL, ERROR, SETUP), componendo un
framebuffer 1-bit a mano (stesso ruolo che aveva PIL) e passandolo a
`esp_lcd_panel_draw_bitmap` (vedi nota su `esp_lcd` sopra). Stesso
linguaggio visivo validato sul Pi, incluso il fix già fatto lì
(radius-clamping) — non lo si riscopre da zero.

**Prerequisito hardware, non bloccante per questa spec**: nessuno dei due
moduli SSD1306 testati finora sul Pi ha mostrato output visibile (con
alimentazione confermata stabile) — va verificato/sostituito prima di
collegare il display alla nuova board. Il firmware viene comunque
progettato per includerlo fin dalla prima milestone, come da decisione
presa in fase di brainstorming.

## Testing

- **Logica pura** (`protocol`, `orchestrator` con interfacce finte):
  compilata ed eseguita sul **target Linux di ESP-IDF**
  (`idf.py --preview set-target linux`), framework **Unity** (incluso in
  ESP-IDF) — stesso spirito della suite pytest di `haro/` (0.7s, nessun
  hardware necessario).
- **Componenti hardware-dipendenti** (audio, display, WiFi): validazione
  manuale su scheda reale, stesso approccio usato oggi sul Pi (test di
  loopback mic→cassa, `i2cdetect`, ecc.).

## Milestone 1 (MVP)

Tutto insieme in un solo colpo, come deciso: WiFi (provisioning
ufficiale) + wake word "Hi ESP" + streaming audio al server esistente +
riproduzione risposta TTS + faccina Cozmo su SSD1306. Nessuna fase
intermedia audio-only.
