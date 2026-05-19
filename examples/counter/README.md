# Counter Example (esp-tinybft)

This example demonstrates how to use `esp-tinybft` to build a simple
distributed counter application over ESP-NOW and UDP (Wi-Fi).

## Architecture

The state of the application is a single integer counter starting at 0.
Clients submit `increment` requests which increase the counter by 1.
Requests are ordered and agreed upon by the BFT replicas before being executed.

## Setup

1. **Generate Configuration and Keys**
   First, generate the RSA keys and configuration files that will be flashed onto the devices:
   ```bash
   ./gen_configs.sh
   ```
   This creates a `spiffs_image` directory containing:
   - RSA private and public keys (`privX.der`, `pubX.der`)
   - `config_udp.txt` and `config_espnow.txt`

2. **Configure the Transport**
   Use `idf.py menuconfig` to configure the transport mechanism and network settings:
   - Navigate to `Component config -> TinyBFT Configuration`
   - Select either **ESP-NOW** or **UDP (lwIP sockets)** under `Transport backend`.
   - If using UDP, navigate to `Counter Example Configuration` and set your Wi-Fi SSID and Password.

3. **Build and Flash**
   You need 8 ESP32 boards (7 replicas to tolerate 2 faults, and 1 client).

   Build and flash:
   ```bash
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

## Note on MACs and IP Addresses

The generated `config_espnow.txt` and `config_udp.txt` contain dummy MAC addresses and loopback IP addresses.
**For a real test across multiple boards, you MUST edit `config_espnow.txt` in the `spiffs_image/` folder**
to contain the real MAC addresses of your ESP32 boards before running `idf.py build`.
