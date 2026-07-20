# Anforderungsdokument

## Empfänger-Simulator-Instanz mit 80-MHz-IQ, 4 × 20-MHz-DDC und Scanner-Betrieb

## 1. Ziel

Ziel ist die Entwicklung eines softwarebasierten Empfänger-Simulators in C. Eine Simulatorinstanz muss nicht alle Kanäle oder Empfängergeräte eines Gesamtsystems abdecken. Stattdessen müssen mehrere Simulatorinstanzen parallel mit derselben Szenariodatei und äquivalenter Geräte- und Port-Konfiguration gestartet werden können und bei gleicher Szenariozeit deterministisch dasselbe RF-Szenario abspielen.

Jedes simulierte Gerät stellt ein 80-MHz-IQ-Basisband (Sample Rate 98.304 MS/s) bereit und unterstützt zusätzlich 4 frei positionierbare 20-MHz-DDC-Kanäle (Sample Rate 24.576 MS/s) innerhalb dieses 80-MHz-Fensters.

Die simulierten Empfänger werden durch externe Systeme über eine REST API kommandiert. IQ-Signalquellen kommen aus IQ-Dateien. Die erzeugten Empfänger-IQ-Daten werden als komplexe `int16`-Samples über UDP bereitgestellt.

Der Simulator ersetzt ausschließlich die Empfängerhardware. Externe Komponenten wie Detector, Classifier, Decoder, DDC-Computer oder Steuerlogik werden nicht simuliert.

---

## 2. Systemumfang

Eine Simulatorinstanz muss folgende Funktionen bereitstellen:

* Simulation einer konfigurierbaren Anzahl unabhängiger Empfänger
* Paralleler Betrieb mehrerer Simulatorinstanzen mit derselben Szenariodatei und instanzspezifischer YAML-Gerätekonfiguration
* Deterministisch gleiche Wiedergabe bei gleicher Szenariodatei und gleicher Szenariozeit
* Einstellbarer Frequenzbereich pro Empfänger innerhalb von 0 bis 40 GHz
* 80-MHz-IQ-Ausgabe pro Empfänger
* 4 bewegliche 20-MHz-DDC-Kanäle pro Empfänger innerhalb des 80-MHz-Fensters
* Scanner-Betrieb mit einer Scanrate von 100 GHz/s
* Wiedergabe und Mischung realer oder vorbereiteter IQ-Signalquellen
* Live-Platzierung von Signalen im aktuellen 80-MHz-Fenster
* Zeit- und frequenzkorrekte Wiedergabe von IQ-Dateien im aktuellen Empfangsfenster
* Kommandierung über REST API
* Ausgabe der IQ-Daten über UDP als komplexe `int16`-Samples

Nicht Bestandteil des Systems:

* Simulation von Detectoren
* Simulation von Classifiern
* Simulation von Decodern
* Simulation externer DDC-Computer
* Simulation der Steuerlogik, die auf Detektionen reagiert

---

## 3. Grundprinzip

Der Simulator basiert auf einer RF-Szenario-Welt. In dieser Welt werden Signale mit absoluter Frequenz, Zeitverhalten, Pegel und IQ-Quelle beschrieben.

Mehrere Simulatorinstanzen können dieselbe RF-Szenario-Welt laden. Wenn sie mit derselben Szenariodatei und derselben Szenariozeit laufen, müssen sie dieselbe Signalplatzierung, dieselben Source-Playback-Positionen und dieselben zeitabhängigen Frequenzverläufe erzeugen.

Jeder simulierte Empfänger erzeugt aus dieser RF-Welt nur den Ausschnitt, den ein echter Empfänger bei seiner aktuellen Mittenfrequenz sehen würde.

Beispiel:

```text
Empfänger-Mittenfrequenz: 10.000 GHz
Empfängerbandbreite:     80 MHz
sichtbarer Bereich:      9.960 GHz bis 10.040 GHz
```

Alle Signale, die in diesen Bereich fallen, werden in das 80-MHz-Basisband gemischt.

Die IQ-Aufzeichnungen sind nicht die vollständige Simulation, sondern Signalquellen innerhalb einer zeit- und frequenzkonsistenten RF-Welt.

### 3.1 Mehrinstanz-Betrieb

Der Mehrinstanz-Betrieb ist ein zentrales Architekturziel.

Eine einzelne Simulatorinstanz muss nicht alle Empfänger, Kanäle oder UDP-Streams eines Gesamtsystems bedienen. Es muss möglich sein, mehrere Instanzen parallel zu betreiben, zum Beispiel auf unterschiedlichen Rechnern oder mit unterschiedlichen Portbereichen.

Damit mehrere Instanzen dasselbe Szenario zeitgleich abspielen können, müssen folgende Voraussetzungen erfüllt sein:

```text
gleiche Szenariodatei
gleiche Receiver-IDs für inhaltlich gleiche Geräte
gleiche REST-Laufzeitkommandos zur gleichen Szenariozeit
instanzspezifische REST- und UDP-Ports
```

Instanzspezifisch dürfen nur Ressourcen wie `instance_id`, Netzwerkadressen, UDP-Ports, CPU-Affinität und Logging-Pfade abweichen. Inhaltliche Parameter des RF-Szenarios, Source-Playback-Positionen und Scan-Verläufe müssen bei gleicher Szenariodatei identisch bleiben.

---

## 4. Betriebsarten

### 4.1 Frequenzbereich pro Gerät

Ein simuliertes Gerät wird über einen gewünschten RF-Frequenzbereich konfiguriert:

```text
frequency_start_hz
frequency_stop_hz
```

Aus diesem Bereich ergibt sich automatisch der Betriebsmodus.

Wenn der Bereich maximal 80 MHz breit ist, arbeitet das Gerät als Fixed-Tune-Empfänger. Die effektive Mittenfrequenz ist dann:

```text
center_frequency_hz = (frequency_start_hz + frequency_stop_hz) / 2
```

Wenn der Bereich größer als 80 MHz ist, arbeitet das Gerät als Scanner.

### 4.2 Scanner-Betrieb

Im Scanner-Betrieb verändert sich die Mittenfrequenz über die Zeit linear innerhalb des konfigurierten Frequenzbereichs.

```text
fc(t) = frequency_start_hz + scan_rate * t
```

Die Standard-Scanrate beträgt:

```text
100 GHz/s
```

Der Scanner kann über einen Frequenzbereich von 0 bis 40 GHz laufen.

Ein Signal mit absoluter Frequenz `f_sig` erscheint im Scanner-Basisband bei:

```text
f_baseband(t) = f_sig - fc(t)
```

Dadurch läuft ein festes RF-Signal während des Scans durch das 80-MHz-Basisband. Dieses Verhalten muss im IQ korrekt abgebildet werden.

### 4.3 Source-Playback / Replay

Replay bedeutet in diesem Dokument das zeitkonsistente Abspielen einer IQ-Datei.

Replay ist kein eigener Empfänger-Modus. Das Gerät arbeitet je nach konfiguriertem Frequenzbereich fixed oder scan. Der Hybrid-Renderer liest die IQ-Datei passend zur globalen Szenariozeit, verschiebt sie an die absolute RF-Position des Signals und mischt sie mit anderen Signalen.

Wenn ein Empfänger später auf ein Signal tuned, beginnt die Quelle nicht automatisch bei Sample 0. Die Dateiposition ergibt sich aus der aktuellen Szenario-Tageszeit, `start_time_s`, `repeat_interval_s` und der IQ-Dateilänge. Dadurch können mehrere Simulatorinstanzen mit derselben Szenariodatei dieselben Source-Playback-Positionen erzeugen.

### 4.4 Hybrid-Rendering-Modus

Der Standardbetrieb ist Hybrid-Rendering:

```text
IQ-Aufzeichnung / Signalquelle
    + absolute RF-Platzierung
    + Live-Frequenzverschiebung
    + Pegelmodell
    = simuliertes Empfänger-IQ
```

Dieser Modus soll die Flexibilität von Live-Simulation mit der Realitätsnähe aufgezeichneter IQ-Signale verbinden.

---

## 5. Empfänger-Modell

Jeder simulierte Empfänger besitzt folgende Eigenschaften:

```text
receiver_id
frequency_start_hz
frequency_stop_hz
scan_rate_hz_per_s
bandwidth_hz = 80000000
sample_rate_hz = 98304000
iq_output_format = ci16
udp_80mhz_output_port
ddc_configuration[4]
```

Ein konfiguriertes Gerät ist immer aktiv. Es gibt im einfachen Startmodell keinen `enabled`-Zustand.

Der Betriebsmodus wird nicht separat konfiguriert:

* `frequency_stop_hz - frequency_start_hz <= 80000000`: Fixed-Tune-Betrieb
* `frequency_stop_hz - frequency_start_hz > 80000000`: Scanner-Betrieb

### 5.1 Frequenzbereich

Jeder Empfänger muss Frequenzbereiche innerhalb des folgenden Gesamtbereichs unterstützen:

```text
0 Hz bis 40 GHz
```

Die genaue Auflösung der Frequenzgrenzen muss mindestens 1 Hz betragen, sofern intern möglich. Die API muss Frequenzen als 64-Bit-Integer in Hz akzeptieren.

### 5.2 80-MHz-Basisband

Jeder Empfänger stellt immer ein komplexes 80-MHz-IQ-Signal bereit.

Die nominelle Bandbreite beträgt:

```text
80 MHz
```

Die Sample Rate beträgt fest:

```text
98.304 MS/s
```

Das Ausgabeformat ist im Startmodell fest:

```text
complex int16, Little Endian, I/Q interleaved
```

Der 80-MHz-IQ-Ausgang besitzt einen eigenen UDP-Port.

### 5.3 4 × 20-MHz-DDC

Jeder Empfänger besitzt immer 4 DDC-Kanäle.

Jeder DDC-Kanal hat:

```text
ddc_id: 0..3
center_frequency_hz als absolute RF-Frequenz
bandwidth_hz = 20 MHz
udp_output_port
```

Die DDC-Kanäle sind im Startmodell immer aktiv. Die Sample Rate ist fest 24.576 MS/s. Ein DDC-Kanal muss innerhalb des aktuellen 80-MHz-Empfangsfensters liegen, damit er sinnvolle IQ-Daten liefert. Wenn die konfigurierte absolute DDC-Frequenz außerhalb des aktuellen Empfangsfensters liegt, muss das Verhalten definiert und sichtbar sein, z. B. leerer Stream oder Fehlerstatus.

---

## 6. IQ-Datenformat

### 6.1 Grundformat

IQ-Daten werden als komplexe `int16`-Samples über UDP übertragen.

Sample-Layout:

```text
I0, Q0, I1, Q1, I2, Q2, ...
```

Datentyp:

```c
int16_t i;
int16_t q;
```

Byte Order:

```text
Little Endian
```

Ein komplexes Sample besteht aus:

```text
4 Byte = 2 Byte I + 2 Byte Q
```

### 6.2 Skalierung

Die IQ-Skalierung muss dokumentiert und konfigurierbar sein.

Empfohlene Standarddefinition:

```text
int16 full scale = ±32767
0 dBFS entspricht maximaler digitaler Aussteuerung
```

Der Simulator muss Clipping bei Übersteuerung realistisch abbilden können.

### 6.3 UDP-Streaming-Ausgabe

Jeder IQ-Ausgang wird über einen eigenen UDP-Port bereitgestellt.

Mindestanforderung:

```text
80-MHz-IQ pro Empfänger über UDP
4 DDC-IQ-Streams pro Empfänger über UDP
```

Die konkreten UDP-Zielports werden pro Gerät und Stream explizit in der YAML-Konfiguration angegeben.

### 6.4 Framing

Standard für UDP-Ausgaben ist Raw UDP ohne Simulator-Header. Das Format soll direkt mit GNU Radio UDP Source verwendbar sein: UDP-Payloads enthalten ausschließlich die fortlaufenden IQ-Samples im konfigurierten Sample-Layout.

Ein zusätzlicher Framed-Modus ist nicht Teil des Startmodells und kann später ergänzt werden.

#### Raw-Modus

```text
kontinuierlicher Strom von int16 I/Q Samples
kein Header
```

Vorteil:

```text
einfach
kompatibel mit vielen bestehenden Tools
```

Nachteil:

```text
keine Zeitstempel
keine Stream-Metadaten
keine Blockgrenzen
```

---

## 7. REST API pro Gerät

Jedes simulierte Gerät besitzt eine eigene REST API. Bind-Adresse und Port werden pro Gerät in der YAML-Gerätekonfiguration festgelegt.

Es gibt keine zentrale REST API, über die mehrere Geräte adressiert werden. Wenn mehrere Geräte simuliert werden, laufen entsprechend mehrere REST-Endpunkte, zum Beispiel:

```text
receiver 0: http://127.0.0.1:8100/api/v1
receiver 1: http://127.0.0.1:8101/api/v1
```

### 7.1 Allgemeine Anforderungen

* HTTP-basiert
* JSON als Request- und Response-Format
* Eindeutige Fehlercodes
* Versionierte API
* Statusabfrage pro Gerät
* Validierung aller Parameter

Basis-Pfad:

```text
/api/v1
```

### 7.2 Health Check

```http
GET /api/v1/health
```

Antwort:

```json
{
  "status": "ok",
  "version": "1.0.0",
  "uptime_s": 1234
}
```

### 7.3 Szenario-Status

Das Szenario wird beim Start aus der Simulator-Konfiguration geladen. Es gibt keine REST-Anforderung, ein Szenario explizit zu starten. Der Endpunkt zeigt den Szenariozustand aus Sicht dieses Geräts.

```http
GET /api/v1/scenario/status
```

Antwort:

```json
{
  "scenario_id": "test_scenario_001",
  "loaded": true,
  "scenario_time_ns": 123456789000,
  "source_count": 1,
  "signal_count": 1
}
```

---

## 8. REST API für das Gerät

### 8.1 Gerätekonfiguration abfragen

```http
GET /api/v1/config
```

Antwort:

```json
{
  "receiver_id": 0,
  "frequency_start_hz": 10000000000,
  "frequency_stop_hz": 10080000000,
  "effective_mode": "fixed",
  "sample_rate_hz": 98304000,
  "bandwidth_hz": 80000000,
  "iq_output_format": "ci16",
  "udp_output_host": "127.0.0.1",
  "udp_outputs": {
    "iq_80mhz": {
      "port": 50000
    },
    "ddc": [
      {
        "ddc_id": 0,
        "port": 50001
      }
    ]
  },
  "ddc": [
    {
      "ddc_id": 0,
      "center_frequency_hz": 10010000000,
      "bandwidth_hz": 20000000,
      "sample_rate_hz": 24576000
    }
  ]
}
```

### 8.2 Frequenzbereich setzen

```http
POST /api/v1/frequency-range
```

Body:

```json
{
  "frequency_start_hz": 10000000000,
  "frequency_stop_hz": 10080000000,
  "scan_rate_hz_per_s": 100000000000
}
```

Anforderungen:

* Frequenzbereich: 0 bis 40 GHz
* `frequency_stop_hz` muss größer als `frequency_start_hz` sein
* Bei Bereich bis 80 MHz arbeitet das Gerät fixed
* Bei Bereich größer als 80 MHz scannt das Gerät über den Bereich
* Scanrate bis mindestens 100 GHz/s
* Die Änderung muss zeitkonsistent in den laufenden Renderer übernommen werden

### 8.3 Gerätestatus

```http
GET /api/v1/status
```

Antwort:

```json
{
  "receiver_id": 0,
  "effective_mode": "scan",
  "frequency_start_hz": 9950000000,
  "frequency_stop_hz": 10050000000,
  "center_frequency_hz": 12345000000,
  "scan_rate_hz_per_s": 100000000000,
  "udp_output_host": "127.0.0.1",
  "udp_outputs": {
    "iq_80mhz": {
      "port": 50000
    }
  },
  "streams_active": true
}
```

---

## 9. REST API für DDC-Kanäle

### 9.1 DDC konfigurieren

```http
POST /api/v1/ddc/{ddc_id}/configure
```

Body:

```json
{
  "center_frequency_hz": 10010000000
}
```

Anforderungen:

* `ddc_id`: 0 bis 3
* `center_frequency_hz` ist eine absolute RF-Frequenz
* Bandbreite nominal 20 MHz
* Sample Rate fest 24.576 MS/s
* Fehler bei ungültiger Konfiguration

### 9.2 DDC-Status

```http
GET /api/v1/ddc/{ddc_id}/status
```

Antwort:

```json
{
  "receiver_id": 0,
  "ddc_id": 0,
  "center_frequency_hz": 10010000000,
  "bandwidth_hz": 20000000,
  "sample_rate_hz": 24576000,
  "udp_output": {
    "port": 50001
  }
}
```

---

## 10. RF-Szenario und Signalquellen

### 10.1 Szenario-Konzept

Das RF-Szenario beschreibt, welche Signale in der simulierten RF-Welt existieren.

Ein Signal besitzt mindestens:

```text
signal_id
source_reference
center_frequency_hz
bandwidth_hz
power_dbm
start_time_s
repeat_interval_s
```

### 10.2 Signalquellen

Signalquellen beschreiben, woher IQ-Sample-Daten kommen. Sie beschreiben noch nicht, wo und wann ein Signal in der RF-Welt auftaucht.

Unterstützte Signalquellen im Startmodell:

* IQ-Dateien

Jede Signalquelle besitzt mindestens:

```text
id
source_type
file
format
sample_rate_hz
bandwidth_hz
center_frequency_hz
byte_order
iq_layout
```

`sample_count` muss bekannt sein. Der Simulator darf `sample_count` aus der Dateigröße ableiten, wenn Format und Sample-Größe eindeutig sind.

#### IQ-Datei

Beispiel:

```json
{
  "id": "asset_fsk_001",
  "source_type": "iq_file",
  "file": "assets/fsk_20mhz.c16",
  "format": "ci16",
  "byte_order": "little_endian",
  "iq_layout": "interleaved_iq",
  "sample_rate_hz": 24576000,
  "bandwidth_hz": 20000000,
  "center_frequency_hz": 0,
  "sample_count": 245760000,
  "nominal_level_dbfs": -12.0
}
```

`center_frequency_hz` beschreibt bei einer IQ-Datei die Frequenzlage innerhalb der Datei, nicht die absolute RF-Frequenz im Szenario.

### 10.3 Signale

Signale platzieren eine Signalquelle in der RF-Welt. Hier werden absolute Frequenz, Leistung und zyklische Zeitplanung definiert.

`start_time_s` ist immer relativ zu 00:00:00 der Szenario-Tageszeit. `repeat_interval_s` definiert, nach wie vielen Sekunden sich das Signalereignis wiederholt. Pro Ereignis wird die IQ-Datei genau einmal ab Dateianfang abgespielt.

Die Szenario-Tageszeit ist:

```text
scenario_day_time_s = Sekunden seit 00:00:00 der Systemzeit, modulo 86400
```

Mehrere Simulatorinstanzen müssen synchronisierte Systemzeit verwenden, damit sie dieselben Signale zur selben Zeit abspielen.

Ein Signal ist aktiv, wenn für ein ganzzahliges `n >= 0` gilt:

```text
file_duration_s = sample_count / sample_rate_hz
occurrence_start_s = start_time_s + n * repeat_interval_s
occurrence_start_s <= scenario_day_time_s < occurrence_start_s + file_duration_s
```

Die Position innerhalb der IQ-Datei wird innerhalb dieser Aktivierung berechnet:

```text
active_time_s = scenario_day_time_s - occurrence_start_s
file_time_s = active_time_s
```

Nach `sample_count / sample_rate_hz` Sekunden ist dieses Signalereignis beendet. Die Datei wird innerhalb desselben Ereignisses nicht wiederholt.

Validierung:

```text
0 <= start_time_s < 86400
repeat_interval_s > 0
sample_count > 0
sample_count / sample_rate_hz <= repeat_interval_s
```

Beispiel:

```json
{
  "signal_id": "sig_fsk_asset",
  "source_reference": "asset_fsk_001",
  "center_frequency_hz": 10005000000,
  "bandwidth_hz": 20000000,
  "power_dbm": -55.0,
  "start_time_s": 0.0,
  "repeat_interval_s": 60.0
}
```

Dieses Beispiel bedeutet: Das Signal startet jeden Tag bei 00:00:00 und danach alle 60 Sekunden erneut. Die IQ-Datei wird pro Ereignis genau einmal abgespielt. Die Ereignisdauer ergibt sich aus `sample_count / sample_rate_hz` der referenzierten Quelle.

Eine Quelle kann mehrfach als Signal verwendet werden, zum Beispiel mit unterschiedlichen RF-Frequenzen, Pegeln oder Zeitplänen.

### 10.4 Beispiel-Szenariodatei

Eine Szenariodatei beschreibt die RF-Welt unabhängig von einer konkreten Simulatorinstanz. Mehrere Simulatorinstanzen können dieselbe Datei laden. Instanzspezifische Werte wie UDP-Portbereiche für Ausgänge gehören in die Simulator-Konfiguration, nicht in das Szenario.

Beispiel `scenarios/scanner_fsk.yaml`:

```json
{
  "schema_version": 1,
  "scenario_id": "test_scenario_001",
  "description": "Scanner-Test mit IQ-Asset",
  "units": {
    "frequency_unit": "Hz",
    "time_unit": "s",
    "level_unit": "dBm"
  },
  "sources": [
    {
      "id": "asset_fsk_001",
      "source_type": "iq_file",
      "file": "assets/fsk_20mhz.c16",
      "format": "ci16",
      "byte_order": "little_endian",
      "iq_layout": "interleaved_iq",
      "sample_rate_hz": 24576000,
      "bandwidth_hz": 20000000,
      "center_frequency_hz": 0,
      "sample_count": 245760000,
      "nominal_level_dbfs": -12.0
    }
  ],
  "signals": [
    {
      "signal_id": "sig_fsk_asset",
      "source_reference": "asset_fsk_001",
      "center_frequency_hz": 10005000000,
      "bandwidth_hz": 20000000,
      "power_dbm": -55.0,
      "start_time_s": 0.0,
      "repeat_interval_s": 60.0
    }
  ]
}
```

Für Mehrinstanz-Synchronität müssen `scenario_id`, Signaldefinitionen und Source-Metadaten identisch sein. Die Szenariozeit ist die Tageszeit relativ zu 00:00:00. Unterschiede in `instance_id`, Ausgangsports oder Logging-Pfaden dürfen nicht Teil der Szenariodatei sein.

---

## 11. Rendering-Anforderungen

### 11.1 80-MHz-Renderer

Der 80-MHz-Renderer muss für jeden aktiven Empfänger bestimmen:

```text
aktuelles Empfangsfenster = center_frequency_hz ± 40 MHz
```

Dann müssen alle aktiven Signale ausgewählt werden, deren Spektrum dieses Fenster schneidet.

Diese Signale werden:

```text
zeitlich korrekt aus der IQ-Quelle gelesen
resampelt, falls erforderlich
frequenzverschoben
im Pegel angepasst
summiert
als int16 IQ ausgegeben
```

### 11.2 Frequenzverschiebung

Für Fixed-Tune:

```text
offset_hz = signal_center_frequency_hz - receiver_center_frequency_hz
```

Für Scanner:

```text
offset_hz(t) = signal_center_frequency_hz - receiver_center_frequency_hz(t)
```

Beim Scanner muss eine zeitabhängige Frequenzverschiebung unterstützt werden.

### 11.3 Zeitkonsistenz

Alle Signalquellen müssen an eine globale Szenariozeit gekoppelt sein.

Wenn ein Empfänger später auf ein Signal tuned, darf das Signal nicht automatisch bei Sample 0 neu beginnen. Es muss entsprechend der aktuellen Szenario-Tageszeit und des periodischen Signalplans gelesen werden.

Für mehrere parallel laufende Simulatorinstanzen gilt zusätzlich:

* Gleiche Szenariodatei und gleiche Szenariozeit müssen zu gleicher Source-Playback-Position führen.
* Scanner-Frequenzen müssen aus der Szenariozeit berechnet werden, nicht aus lokaler Thread-Laufzeit.

### 11.4 DDC-Rendering

DDC-Ausgänge werden direkt aus dem RF-Szenario gerendert.

```text
RF-Szenario -> direktes 20-MHz-IQ
```

Der direkte Pfad muss Frequenzlage, Pegel, Zeitplanung und IQ-Dateiposition konsistent zum 80-MHz-Renderer berechnen.

---

## 12. Erweiterungen

Ein erweitertes Frontend-Modell ist nicht Teil des einfachen Startmodells.

Mögliche spätere Erweiterungen sind Rauschboden, Gain, Frequenzoffset, Sample-Rate-Offset, IQ-Imbalance, DC-Offset, Clipping, Frontend-Filter und Tuner-Settling. Diese Erweiterungen dürfen die Grundsemantik von `frequency_start_hz`, `frequency_stop_hz`, DDC-`center_frequency_hz` und den festen Sample-Rates nicht verändern.

---

## 13. Performance-Anforderungen

### 13.1 Anzahl Empfänger

Eine Simulatorinstanz muss bis zu 12 Empfänger verwalten können.

Nicht alle Empfänger müssen zwingend gleichzeitig mit voller 80-MHz-Rate aktiv sein, sofern dies als Konfigurationslimit dokumentiert wird. Größere Gesamtsysteme sollen durch mehrere parallel gestartete Simulatorinstanzen abgedeckt werden können.

### 13.2 Datenrate

Bei komplexem `int16`:

```text
1 Sample = 4 Byte
```

Beispiel bei 98.304 MS/s:

```text
pro 80-MHz-Stream: 393.216 MB/s
12 Streams:        4.718592 GB/s
```

Beispiel bei 24.576 MS/s:

```text
pro 20-MHz-DDC-Stream: 98.304 MB/s
48 DDC-Streams:        4.718592 GB/s
```

zusätzliche DDC-Streams erhöhen die Datenrate entsprechend.

Die Architektur muss daher:

* Zero-Copy- oder Low-Copy-Pfade bevorzugen
* Ringbuffer verwenden
* UDP-Backpressure behandeln
* Stream-Ausfülle erkennen
* nicht benötigte Streams nicht berechnen
* nur sichtbare Signale rendern
* Caching und Prerendering unterstützen

### 13.3 Echtzeitfühigkeit

Der Simulator muss in Echtzeit laufen, weil die IQ-Daten kontinuierlich per UDP an externe Abnehmer ausgegeben werden.

Der gleichzeitige Betrieb von 12 Empfängern muss unterstützt werden, darf aber auf mehrere Rechner oder mehrere Simulatorinstanzen verteilt werden. Die Implementierung soll so schnell wie möglich sein; die endgültige Zielhardware wird anhand der erreichten Performance bestimmt.

---

## 14. Software-Architektur in C

### 14.1 Programmiersprache

Die Implementierung erfolgt in C.

Empfohlener Standard:

```text
C11 oder neuer
```

### 14.2 Module

Vorgeschlagene Modulstruktur:

```text
src/
  main.c
  config.c
  rest_server.c
  udp_output.c
  receiver.c
  receiver_manager.c
  scenario.c
  signal_asset.c
  iq_file_reader.c
  renderer_80mhz.c
  renderer_ddc.c
  nco.c
  resampler.c
  fir_filter.c
  ringbuffer.c
  timebase.c
  sync_control.c
  logging.c
  metrics.c
```

### 14.3 Zentrale Datenstrukturen

Beispiel:

```c
typedef struct {
    uint16_t port;
} udp_output_config_t;

typedef struct {
    uint32_t id;
    uint64_t center_frequency_hz;
    uint32_t bandwidth_hz;
    uint32_t sample_rate_hz;
    udp_output_config_t udp_output;
} ddc_config_t;

typedef struct {
    uint32_t id;

    uint64_t frequency_start_hz;
    uint64_t frequency_stop_hz;
    uint64_t bandwidth_hz;
    uint32_t sample_rate_hz;
    double scan_rate_hz_per_s;

    char udp_output_host[64];
    udp_output_config_t udp_80mhz_output;
    ddc_config_t ddc[4];
} receiver_config_t;

typedef struct {
    int16_t i;
    int16_t q;
} iq_ci16_t;
```

Zusätzlich muss es eine globale Szenariozeitbasis geben, die unabhängig von lokalen Thread-Zeitpunkten verwendbar ist:

```c
typedef struct {
    bool running;
} simulation_control_t;
```

---

## 15. Nebenläufigkeit

Das System muss mehrere parallele Aufgaben ausführen:

* REST API Threads pro konfiguriertem Gerät oder gemeinsamer HTTP-Server mit getrennten Ports
* Empfänger-Management
* Rendering-Threads
* UDP-Streaming-Threads
* Asset-I/O
* Logging/Metrics

Empfohlenes Modell:

```text
1 Control Thread
REST Thread Pool für die Geräte-APIs
N Renderer Worker Threads
1 UDP-Output Thread pro aktivem Stream oder Event-Loop
Shared Ringbuffers zwischen Renderer und UDP
```

REST-Kommandos dürfen nicht direkt in laufende Renderbuffer schreiben. Stattdessen sollen Konfigurationsänderungen atomar oder über Command Queues übernommen werden.

---

## 16. Fehlerbehandlung

Das System muss Fehler eindeutig melden.

Beispiele:

```text
invalid_receiver_id
invalid_frequency
invalid_ddc_offset
stream_not_active
asset_not_found
scenario_invalid
udp_client_disconnected
render_overrun
buffer_underrun
unsupported_sample_rate
```

REST-Fehlerantwort:

```json
{
  "error": {
    "code": "invalid_frequency",
    "message": "frequency_start_hz and frequency_stop_hz must be between 0 and 40000000000"
  }
}
```

---

## 17. Logging und Metriken

Das System muss mindestens folgende Informationen loggen:

* Start/Stop des Simulators
* REST-Kommandos
* Frequenzwechsel
* Scan-Start/Stop
* DDC-Konfigurationsänderungen
* UDP-Verbindungen
* Stream-Abbrüche
* Buffer Overruns/Underruns
* Rendering-Auslastung
* Szenario-Ladevorgänge
* Asset-Fehler

Metriken:

```text
samples_rendered
samples_dropped
udp_bytes_sent
render_cpu_load
ringbuffer_fill_level
active_receivers
active_streams
current_center_frequency
scan_position
```

Optionaler Endpunkt:

```http
GET /api/v1/metrics
```

---

## 18. Konfiguration

Der Simulator muss über eine YAML-Konfigurationsdatei startbar sein. Diese Konfiguration beschreibt die lokale Simulatorinstanz, die zu ladende Szenariodatei, die REST-Endpunkte pro Gerät und die expliziten UDP-Ausgabeports.

Das Szenario selbst bleibt eine separate Datei und beschreibt nur die RF-Welt. Pfade wie `scenario_file` und IQ-Dateien im Szenario müssen absolut sein oder relativ zur YAML-Datei aufgelöst werden.

Beispiel `config/simulator.yaml`:

```yaml
instance_id: sim-a

scenario_file: scenarios/scanner_fsk.yaml

receivers:
  - receiver_id: 0
    rest:
      bind_address: 127.0.0.1
      port: 8100
    udp_output_host: 127.0.0.1
    udp_outputs:
      iq_80mhz:
        port: 50000
      ddc:
        - ddc_id: 0
          port: 50001
        - ddc_id: 1
          port: 50002
        - ddc_id: 2
          port: 50003
        - ddc_id: 3
          port: 50004

  - receiver_id: 1
    rest:
      bind_address: 127.0.0.1
      port: 8101
    udp_output_host: 127.0.0.1
    udp_outputs:
      iq_80mhz:
        port: 50010
      ddc:
        - ddc_id: 0
          port: 50011
        - ddc_id: 1
          port: 50012
        - ddc_id: 2
          port: 50013
        - ddc_id: 3
          port: 50014
```

Alle Geräte, REST-Endpunkte und UDP-Ausgabeports müssen explizit in der YAML-Datei stehen. Es gibt kein implizites Portschema und keine implizit erzeugten Empfänger.

Frequenzbereich, Scanrate und DDC-Mittenfrequenzen sind Laufzeitkonfiguration und werden nicht in der YAML-Datei gespeichert. Sie werden nach dem Start über die REST API des jeweiligen Geräts gesetzt.

---

## 19. Akzeptanzkriterien

### 19.1 Grundfunktion

* Simulator startet erfolgreich mit 12 konfigurierten Empfängern.
* Simulator lädt beim Start die YAML-Konfiguration und die darin referenzierte Szenariodatei.
* Die REST API jedes konfigurierten Geräts ist unter der konfigurierten IP und dem konfigurierten Port erreichbar.
* Frequenzbereiche zwischen 0 und 40 GHz können pro Gerät gesetzt werden.
* 80-MHz-IQ wird über UDP als `int16 I/Q` ausgegeben.
* DDC-Kanäle können über absolute RF-Frequenzen konfiguriert werden.
* DDC-IQ wird über UDP bereitgestellt.

### 19.2 Scanner

* Ein Empfänger kann mit 100 GHz/s scannen.
* Die aktuelle effektive Mittenfrequenz ist per Geräte-REST-API abfragbar.
* Ein festes RF-Signal läuft während des Scans korrekt durch das 80-MHz-Basisband.
* Wiederholtes Scannen über definierte Bereiche funktioniert.

### 19.3 Signalrealismus

* IQ-Assets werden zeitkonsistent gelesen.
* Frequenzverschiebung und Pegel stimmen innerhalb definierter Toleranzen.
* Der externe Detector kann auf simulierte IQ-Daten reagieren.
* Externe Classifier/Decoder können mit geeigneten Signalassets arbeiten.

### 19.4 Stabilität

* UDP-Ausgabeprobleme führen nicht zum Absturz.
* Ungültige REST-Kommandos werden sauber abgelehnt.
* Ringbuffer-Überläufe werden erkannt und geloggt.
* Simulator kann mehrere Stunden stabil laufen.

### 19.5 Mehrinstanz-Synchronität

* Zwei Simulatorinstanzen können mit derselben Szenariodatei und instanzspezifischen YAML-Konfigurationen parallel gestartet werden.
* Beide Instanzen erzeugen bei gleicher Szenariozeit dieselben Source-Playback-Positionen, Scan-Frequenzen und Signalplatzierungen.
* Instanzspezifische Parameter wie Portbereiche und Logging-Pfade ändern den simulierten RF-Inhalt nicht.

---

## 20. Offene Punkte

Folgende Punkte müssen vor Implementierungsstart final entschieden werden:

1. Exaktes IQ-Dateiformat: zunächst raw `ci16`; zu klären ist nur, ob ergänzende Metadaten ausschließlich im Szenario stehen oder zusätzlich als Sidecar-Datei erlaubt sind.
2. Filtermodell für direkt gerenderte 20-MHz-DDC-Ausgänge.
3. Zielhardware nach ersten Performance-Messungen festlegen.

Bereits entschieden:

```text
UDP-Ausgabe: Raw UDP ohne Header, kompatibel mit GNU Radio UDP Source
12 Empfänger: gleichzeitig erlaubt, aber auf mehrere Rechner/Instanzen verteilbar
DDC-Rendering: direkt aus dem RF-Szenario
Mehrere UDP-Abnehmer: später über Multicast, nicht Teil des Startmodells
REST TLS/Auth: nicht erforderlich
Zeitquelle: Systemzeit
UDP-Paketverlust/langsame Abnehmer: Best-Effort, keine Garantie im Startmodell
```

---

## 21. Empfohlene MVP-Umsetzung

Der erste MVP sollte bewusst klein sein:

```text
1 bis 2 Empfänger
2 parallel gestartete Simulatorinstanzen mit derselben Szenariodatei
1 Gerät mit Frequenzbereich größer als 80 MHz
1 Gerät mit Frequenzbereich bis 80 MHz
1 80-MHz-UDP-Stream
4 DDC-Konfigurationen
4 DDC-UDP-Streams pro Gerät
IQ-Asset-Playback
Live-Frequenzverschiebung
REST-Steuerung pro Gerät
Start über YAML-Konfiguration mit gemeinsamer Szenariodatei
Logging
```

Danach Skalierung auf:

```text
12 Empfänger
alle DDC-Streams
mehrere Signalquellen
Caching
Performance-Optimierung
```

---

## 22. Zusammenfassung

Der Empfänger-Simulator soll eine reale Empfängerhardware funktional ersetzen. Er wird über REST kommandiert und liefert simulierte IQ-Daten über UDP. Die Simulation basiert auf einer RF-Szenario-Welt mit IQ-Dateien als Signalquellen. Jeder Empfänger rendert daraus sein aktuelles 80-MHz-Fenster und 4 bewegliche 20-MHz-DDC-Ausgänge.

Die Implementierung erfolgt in C mit Fokus auf Performance, deterministisches Verhalten, Mehrinstanz-Synchronität, Echtzeitfühigkeit und realistisches IQ-Verhalten für externe Detector-, Classifier- und Decoder-Systeme.
