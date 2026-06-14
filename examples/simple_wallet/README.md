# Simple Wallet Example (esp-tinybft)

This example demonstrates how to use `esp-tinybft` to build a simple distributed wallet application over ESP-NOW and UDP (Wi-Fi).

## Architecture

The state of the application consists of 4 integer balances.
Clients can submit `transfer` requests which subtract from one balance and add to another. The requests are ordered and agreed upon by the BFT replicas before being executed.

## Setup

1. **Generate Configuration and Keys**
   First, generate the ECDSA P-256 keys and configuration files that will be flashed onto the devices. Run the provided script on your host machine:
   ```bash
   ./gen_configs.sh
   ```
   This creates a `spiffs_image` directory containing:
   - ECDSA P-256 private and public keys (`privX.der`, `pubX.der`)
   - `config_udp.txt` and `config_espnow.txt`

2. **Configure the Transport**
   Use `idf.py menuconfig` to configure the transport mechanism and network settings:
   - Navigate to `Component config -> TinyBFT Configuration`
   - Select either **ESP-NOW** or **UDP (lwIP sockets)** under `Transport backend`.
   - If using UDP, navigate to `Example Connection Configuration` and set your Wi-Fi SSID and Password.

3. **Build and Flash**
   You need 5 ESP32 boards (4 replicas to tolerate 1 fault, and 1 client).

   For each board, you must edit `main.c` before flashing to select its identity:
   - For **Node 0** (Replica): Keep `xTaskCreate(replica_task, ...)` uncommented, and ensure `priv_config` is set to `/spiffs/priv0.der`.
   - For **Node 1-3** (Replicas): Change `priv_config` to `/spiffs/priv1.der`, `/spiffs/priv2.der`, etc. Also set the `TBFT_NODE_ID` environment variable or modify the library to pick up the correct node id for your MAC/IP. In this example, it uses node `0` by default. You can modify `parse_config` in `esp-tinybft` or pass it properly.
   - For **Node 4** (Client): Comment out `replica_task` and uncomment `xTaskCreate(client_task, ...)`. Ensure `priv_config` is `/spiffs/priv4.der`.

   Build and flash:
   ```bash
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

## Note on MACs and IP Addresses

The generated `config_espnow.txt` and `config_udp.txt` contain dummy MAC addresses (`02:00:00:00:00:00`) and loopback IP addresses (`127.0.0.1`).
**For a real test across multiple boards, you MUST edit `config_udp.txt` and `config_espnow.txt` in the `spiffs_image/` folder** to contain the real MAC addresses or IP addresses of your ESP32 boards before running `idf.py build`.
