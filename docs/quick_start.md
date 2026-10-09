# SDR Simulator — Quick Start

The SDR simulator acts like a real SDR receiver. It generates radio signals (noise plus
recorded signals), streams them as VITA-49 IQ data over UDP, and can be retuned at runtime
through a REST API.

This guide covers three things:

1. [Installing the RPM](#1-install)
2. [Running the configurations provided to you](#2-run-the-provided-configurations)
3. [Controlling the simulator through the REST API](#3-control-it-through-the-rest-api)

Requirements: an x86-64 Fedora machine with `sudo` rights. Commands use `curl` to talk to the
REST API.

---

## 1. Install

Download `sdr-simulator-<version>.x86_64.rpm` from the project's GitHub **Releases** page, then:

```sh
sudo dnf install ./sdr-simulator-*.x86_64.rpm
```

Check that it works. With no configuration, the simulator runs a built-in test setup that
streams plain noise:

```sh
sdr-simulator
```

Leave it running. In a **second terminal**:

```sh
curl http://127.0.0.1:8100/api/v1/health
```

Expected answer:

```json
{"status":"ok","version":"0.1.0","uptime_s":3}
```

Stop the simulator with `Ctrl+C`. The installation is working.

To uninstall later: `sudo dnf remove sdr-simulator`.

---

## 2. Run the provided configurations

Each configuration is a **setup folder**, a self-contained directory:

```
<setup>/
├── receiver.yaml    # the receiver: frequencies, channels, network ports
├── scenario.yaml    # the signals being simulated
└── assets/          # signal recordings used by the scenario
```

You do not need to edit these files to use them.

### Provided setups

<!-- FILL IN: one row per setup folder you hand over. -->

| Setup folder | What it simulates | REST port | UDP ports (channel → port) |
| --- | --- | --- | --- |
| `example-setup` | _describe the scenario_ | 8100 | 0 → 50000, 1 → 50001 |

### Step 1 — copy the setup to the machine

Put the folders anywhere you like, for example under `/srv/sim/`:

```sh
sudo mkdir -p /srv/sim
sudo cp -r example-setup /srv/sim/
```

### Step 2 — try it in the foreground

```sh
sdr-simulator --config-dir /srv/sim/example-setup
```

It prints one line per receiver with its REST address and keeps running until `Ctrl+C`. If
something is wrong with the setup (for example a missing recording), it stops immediately
and says what is missing.

### Step 3 — run it permanently as a service

A service starts automatically at boot and restarts after a crash. Each setup gets a short
name, here `example`:

```sh
sudo mkdir -p /etc/sdr-simulator/instances
echo SDR_SIMULATOR_CONFIG_DIR=/srv/sim/example-setup | sudo tee /etc/sdr-simulator/instances/example.conf
sudo systemctl enable --now sdr-simulator@example
```

| To… | Run |
| --- | --- |
| see whether it runs | `systemctl status sdr-simulator@example` |
| read its messages and errors | `journalctl -u sdr-simulator@example` |
| reload after changing the setup files | `sudo systemctl restart sdr-simulator@example` |
| stop it permanently | `sudo systemctl disable --now sdr-simulator@example` |

The service runs as the user `sdr-simulator`, which must be able to read the setup folder. Files
copied with `cp` as above are readable by default.

### Several setups at the same time

Repeat step 3 with a different name and folder for each setup. Setups running at the same time
must use **different REST and UDP ports**. The table above lists the ports of each provided
setup; if two collide, the second one fails to start with `failed to start REST server`.

### Receiving the data on another machine

The provided setups send to the addresses listed in their `receiver.yaml`
(`udp_output_host`, `udp_output_port`) and accept REST calls on `rest_bind_host`. `127.0.0.1`
means this machine only. To receive or control from another machine:

- set `rest_bind_host: "0.0.0.0"` and `udp_output_host` to the receiving machine's address,
  then restart the simulator, and
- open the REST port in the firewall, e.g. `sudo firewall-cmd --add-port=8100/tcp`.

The format of the UDP stream is described in `vita49_udp.md`, next to this guide.

---

## 3. Control it through the REST API

Each receiver of a running simulator has its own REST address,
`http://<host>:<rest_port>/api/v1`. The examples use the built-in test setup
(`127.0.0.1:8100`); replace host and port for your setup. Requests and answers are JSON.

### See what the simulator is doing

| Question | Request |
| --- | --- |
| Is it alive? | `curl http://127.0.0.1:8100/api/v1/health` |
| What can this receiver do (frequency range, number of channels)? | `curl http://127.0.0.1:8100/api/v1/capabilities` |
| Which channels exist, how are they tuned, which rates can they use? | `curl http://127.0.0.1:8100/api/v1/channels` |
| Is it streaming in real time? | `curl http://127.0.0.1:8100/api/v1/metrics` |

A **channel** is one output stream with its own UDP port. Part of a `/channels` answer:

```json
{
  "channel_id": 1,
  "center_frequency_hz": 100000000,
  "bandwidth_hz": 200000,
  "sample_rate_hz": 256000,
  "rates": [ { "bandwidth_hz": 200000, "sample_rate_hz": 256000 } ],
  "stream_enabled": true,
  "udp_port": 50001
}
```

In `/metrics`, `actual_sample_rate_sps` should be close to the sum of the channels' sample
rates, and `samples_missed` should stay at or near 0. If it keeps rising, the machine is
too slow for the setup.

### Change settings

**Tune a channel to another frequency** (in Hz; here 102.5 MHz):

```sh
curl -X PUT http://127.0.0.1:8100/api/v1/channels/1 -d '{"center_frequency_hz": 102500000}'
```

Channels showing `"track_tuner": true` follow the receiver's tuner and cannot be tuned on
their own. Retune the receiver instead (below), or send `"track_tuner": false` together with
the frequency.

**Change a channel's bandwidth.** Always send bandwidth and sample rate together, as one of
the pairs listed in that channel's `rates`:

```sh
curl -X PUT http://127.0.0.1:8100/api/v1/channels/0 -d '{"bandwidth_hz": 5000000, "sample_rate_hz": 6144000}'
```

**Switch a channel's stream off or on:**

```sh
curl -X POST http://127.0.0.1:8100/api/v1/channels/1/stream -d '{"enabled": false}'
curl -X POST http://127.0.0.1:8100/api/v1/channels/1/stream -d '{"enabled": true}'
```

**Retune the receiver** (the frequency range it covers):

```sh
curl -X POST http://127.0.0.1:8100/api/v1/frequency-range -d '{"frequency_min_hz": 95000000, "frequency_max_hz": 105000000}'
```

If the range is wider than the receiver's bandwidth, the receiver sweeps across it; the
answer shows `"effective_mode": "scan"` instead of `"fixed"`.

A successful change answers with the new state. A rejected change answers with an error and
changes nothing, e.g.:

```json
{"error":{"code":"unsupported_channel_rate","message":"requested {bandwidth_hz, sample_rate_hz} is not one of this channel's rates"}}
```

> Changes made through the REST API last until the simulator restarts. A restart always
> goes back to the settings in the setup folder.

The complete endpoint list is in `rest_api.md`, next to this guide.

---

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `asset_not_found: …/assets/x.c16` | A recording is missing from the setup's `assets/` folder. Copy it there. |
| `no setup at … -- create one with: sdr-simulator --init …` | The path given to `--config-dir` (or in the service's `.conf` file) does not contain a `receiver.yaml`. Check the path. |
| `failed to start REST server for receiver 0` | The REST port is already in use, usually by another simulator. Stop the other one or give the setups different ports. |
| Service shows `inactive (dead)` with a condition failure | `/etc/sdr-simulator/instances/<name>.conf` does not exist or the name is misspelled. |
| `curl: (7) Failed to connect` | The simulator is not running, or listens on another host/port. See `systemctl status` and the setup's `rest_bind_host`/`rest_port`. |

## More documentation

Installed in `/usr/share/doc/sdr-simulator/`:

| File | Content |
| --- | --- |
| `quick_start.md` | This guide. |
| `configuration_manual.md` | Writing your own setups: receivers, channels, scenarios, signals. |
| `rest_api.md` | All REST endpoints and fields. |
| `vita49_udp.md` | The UDP stream format, for building a receiver. |
