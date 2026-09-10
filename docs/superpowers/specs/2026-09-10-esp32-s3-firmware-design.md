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
ES7210 + ampli MP1605GTF-Z), espansore GPIO (TCA9555), RTC, gestione
batteria.

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

## Hardware target

Waveshare ESP32-S3-AUDIO-Board:
- ESP32-S3R8 (dual-core, 8MB PSRAM, 16MB flash)
- Codec **ES8311** (playback) + **ES7210** (mic array, con cancellazione
  eco) — bus I2C interno alla board, non condiviso con l'header esterno
- Ampli di potenza **MP1605GTF-Z**
- Espansore GPIO **TCA9555** (per pulsanti/LED onboard)
- Header 18 pin: 11 GPIO liberi (GPIO3-11, GPIO19-20) + 3 EXIO (via
  TCA9555, I2C, più lenti) + alimentazione (5V, 3V3, GND)
- Display **SSD1306 esterno** (riuso dell'hardware già nel progetto Pi),
  collegato su 2 dei GPIO liberi dell'header come bus I2C dedicato (es.
  GPIO8=SDA, GPIO9=SCL), separato dal bus I2C interno della board

## Componenti (mappatura su moduli nativi ESP-IDF/Espressif)

| Component progetto | Componente nativo/ufficiale | Responsabilità nostra |
|---|---|---|
| `audio_pipeline` | `esp_codec_dev` (Espressif) | Configurazione specifica della board (I2C addr, pin I2S) |
| `wake_word` | ESP-SR (AFE + WakeNet "Hi ESP" + VAD, pacchetto ufficiale Espressif) | Collegamento tra AFE e orchestrator (code FreeRTOS) |
| `protocol` | cJSON (incluso in ESP-IDF) | Schema messaggi: `hello`, `end_of_speech`, `emotion`, `response_end`, `error` — identico a `haro/src/haro/protocol.py` |
| `server_client` | `esp_websocket_client` (componente managed ufficiale) | Riconnessione con backoff esponenziale, stessa logica di `server_client.py` |
| `face_display` | `espressif/ssd1306` (componente ufficiale dal registro) | Porting della logica di disegno "Cozmo-style" da `face_display.py` (occhi a pillola inclinabili, bocca ad arco con estremità arrotondate) |
| `wifi_provisioning` | Wi-Fi Provisioning Manager (ufficiale ESP-IDF, SoftAP + web) | Configurazione (SSID hotspot, ecc.) |
| `config` | NVS (storage nativo ESP-IDF) | Schema chiavi (`server_url`, ecc.), sostituisce il file `config.json` del Pi |
| `orchestrator` | — (logica di business specifica di Haro) | Intera macchina a stati, porting diretto di `orchestrator.py` |

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
SPEAKING_HAPPY/SAD/CONFUSED/NEUTRAL, ERROR, SETUP) usando le primitive di
disegno del componente `espressif/ssd1306` invece di PIL. Stesso
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
