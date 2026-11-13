# แผนงานแก้ไข: ความแฟร์ของการวัดพลังงานและความเสถียรของ connection (`examples/counter`)

| | |
|---|---|
| วันที่ | 2026-11-11 |
| อ้างอิงโค้ด | commit `0a7b8ee` บน branch `devlope/v0.2.9` (component version 0.2.9) |
| ที่มา | ผลรีวิวโค้ดโดย Claude Code: อ่านโค้ด, `sdkconfig` ที่ใช้ build ล่าสุด และเอกสาร/ซอร์ส ESP-IDF v6.1 |
| สถานะ | ยังไม่ได้แก้โค้ดใด ๆ และยังไม่ได้ยืนยันด้วยการ build หรือรันบนบอร์ด |

เลขบรรทัดทั้งหมดอ้างอิง commit ข้างต้น ข้อที่กำกับว่า **(อนุมาน)** มาจากการไล่โค้ดและการคำนวณ ต้องยืนยันด้วย log หรือการวัดก่อนนำไปใช้ในรายงาน โค้ดตัวอย่างในเอกสารเป็นโครงร่าง ยังไม่ได้ compile

## 1. สรุปสำหรับคนลงมือแก้

การทดลองต้องการเทียบพลังงานของ TinyBFT (7 replica + 1 client, f=2, ESP32-C3) ใน 3 แบบ: ESP-NOW, UDP + `WIFI_PS_MIN_MODEM` และ UDP + `WIFI_PS_MAX_MODEM` ตามโค้ดปัจจุบัน ผลเทียบยังไม่แฟร์ด้วยเหตุผลหลัก 3 ข้อ

1. **โหมดวิทยุไม่เท่ากัน** ESP-NOW build รันเป็น softAP ซึ่งเปิด RF ตลอดเวลา ส่วน UDP PS ปิด RF เป็นช่วง ผลต่างที่วัดได้จึงมาจากโหมดวิทยุ ไม่ใช่จาก transport
2. **CPU ไม่เคยหลับในทุก variant** `PS_MIN` กับ `PS_MAX` จึงต่างกันเพียงราว 3% ขณะ idle ตามตัวเลขของ Espressif (ดูภาคผนวก)
3. **สอง transport ต่างกันในสิ่งที่ไม่เกี่ยวกับ protocol** ได้แก่ delay คงที่, ขนาดคิวรับ, timeout ใน config, ปริมาณ log และจังหวะการส่ง request

นอกจากนี้ทั้งสอง transport มีจุดที่ทิ้งข้อความโดยไม่มี log หรือตัวนับ จึงไม่รู้ว่า run ไหนมี loss

**งานถือว่าเสร็จเมื่อ**

- ทุก variant build จากไฟล์ config ที่อยู่ใน git และ boot log พิสูจน์ได้ว่าแต่ละบอร์ดรัน variant ไหน
- ค่า protocol (timeout, ขนาดคิว, log level, คาบ request) เท่ากันทุก variant
- ทุกจุดที่ทิ้งข้อความมีตัวนับ และ run ปกติ 30 นาทีต่อ variant ไม่มี request ล้มเหลว
- มีคู่เทียบแบบเดียวกัน คือ ESP-NOW (RF เปิดตลอด) กับ UDP `PS_NONE`

## 1.1 สถานะการทำงาน (อัปเดตหลังลงมือแก้บน `fix/develop/v0.2.9`)

Build ผ่านแบบไม่มี warning ล่าสุด (2026-11-13, checkout ใหม่ที่ไม่มี `sdkconfig` ค้างในเครื่อง) สำหรับ `counter`: espnow (replica), espnow_ps (client), udp_psnone (client), จุด sweep `enps_r12_w110i200`, `udp_max_l3_a20`, `enb_r12` (replica), `udp_min_mcast` (client) และ `simple_wallet` ทุก build ได้ target esp32c3 และไม่มี `CONFIG_PM_ENABLE` เครื่องมือ `tools/flash.py`, `tools/sweep.py`, `tools/analyze.py` ทดสอบด้วยข้อมูลจำลอง **ยังไม่ได้ทดสอบบนบอร์ดจริง** เกณฑ์ตรวจรับที่ต้องใช้บอร์ดยังเป็นของคนทดสอบ

| งาน | สถานะ | หมายเหตุ |
|---|---|---|
| A1 | ทำแล้ว | Kconfig `TBFT_ESPNOW_IF_STA` (ค่าเริ่มต้น) / `_AP`; ต้อง flash ใหม่ทุกบอร์ด |
| A2 | ทำแล้ว | `EXAMPLE_WIFI_LISTEN_INTERVAL`, variant `udp_psnone`, `BANNER` พิมพ์ทุกค่าที่กำหนดจุดทดลอง (PS, listen interval, rate, retry, wake window/interval, broadcast/multicast, channel, PM, tick rate) |
| A3 | ทำแล้ว | ใช้ค่ากลาง VC=30000 ms; **เจ้าของงานวิจัยยังต้องยืนยันค่านี้** |
| A4 | ทำแล้ว | `LWIP_UDP_RECVMBOX_SIZE=16` |
| A5 | ทำแล้ว | คาบสัมบูรณ์ + `CLIENT_STATS` |
| A6 | ทำแล้ว | `sdkconfig.measure` ตั้ง log WARN; ลบการพิมพ์ key |
| A7 | ทำแล้ว | overlay + `build_variant.sh` + README; `sdkconfig.defaults` กำหนด `CONFIG_IDF_TARGET="esp32c3"` ตามวิธีใน `idf-py.rst` ของ ESP-IDF (เดิม build dir ใหม่ใน clone ใหม่ได้ target esp32 เพราะ `targets.cmake` เดา target จาก `sdkconfig` ที่ค้างในเครื่องซึ่ง git ignore ไว้); `tools/flash.py` เลือก image ตาม role จาก MAC ของบอร์ด และ `flash_replicas.fish` เป็น wrapper ของมัน (ทดสอบด้วย esptool จำลอง ยังไม่ได้ flash บอร์ดจริง) |
| A8 | ทำแล้ว | `TBFT_STATS` ทุก 60 s (replica) / ต่อ request (client); UDP นับ drop ของ lwIP ไม่ได้ |
| A9 | ทำแล้ว (ไม่ได้ทดสอบบนบอร์ด) | variant `espnow_ps` = power saving ของ ESP-NOW เอง (modem sleep, ไม่ใช้ light sleep) ผู้ส่ง retry ทุก `TBFT_ESPNOW_TX_RETRY_DELAY_MS` (< window) จนได้ ACK หรือครบ `TBFT_ESPNOW_TX_RETRY_SPAN_MS` (≥ interval) แบบนับเวลา ไม่ใช่นับครั้ง; หลักเดียวกับ application note ของ coin-cell button ของ Espressif (รายละเอียดใน A9) อัตราส่งสำเร็จจริงต้องวัดบนบอร์ด |
| A10 | ทำแล้ว | `TBFT_ESPNOW_PHY_RATE` (0 = ค่าเริ่มต้น IDF 1 Mbps) |
| A11 | ทำแล้ว แต่**อยู่นอกการเปรียบเทียบ best-vs-best** | เจ้าของงานวิจัยกำหนด (2026-11-13) ว่า best-vs-best ใช้เฉพาะ mode ของวิทยุ ไม่พึ่ง light sleep จึงไม่มีจุดใดใน `tools/sweep.py` ใช้ `sdkconfig.pm` ทุกบอร์ดรัน CPU 160 MHz คงที่ สิ่งที่ทำไว้ยังใช้กับการทดลองแยกได้: รอบแรกใช้ไม่ได้จริง: replica ตื่นทุก 20 ms จึงไม่ถึงเกณฑ์เข้า light-sleep ที่ tick 100 Hz ตอนนี้รอได้ถึง `TBFT_IDLE_WAIT_MS` (500 ms) และ `sdkconfig.pm` ใช้ชุดค่าที่ Espressif แนะนำสำหรับ ESP32-C3 (tick 1000 Hz, ปิด GPIO/MAC-BB/CPU ตอนหลับ) ข้อควรระวัง: พอร์ต USB Serial/JTAG ใช้ไม่ได้ระหว่าง light-sleep |
| B1 | ทำแล้ว | รอ send callback ต่อเฟรม, retry 3 ครั้ง, ลบ delay คงที่ |
| B2 | ทำแล้ว | callback ไม่มี lock/log, fast path เฟรมเดียว |
| B3 | ทำแล้ว | `TBFT_MS_TO_TICKS_MIN1` |
| B4 | ทำแล้ว | retry เมื่อ `ENOMEM`/`ENOBUFS` |
| B5 | ทำแล้ว (ไม่ได้ทดสอบบนบอร์ด) | retry แล้ว reboot เมื่อหา id ไม่ได้หรือ init ล้มเหลว (เช่น flash ผิด role), backoff ใน Wi-Fi handler, และ `EXAMPLE_STATIC_IP` ที่ตั้ง IP ตอน `WIFI_EVENT_STA_CONNECTED` ตามลำดับของ `examples/protocols/static_ip` ใน ESP-IDF |
| B6 | ทำแล้ว | ลอง key ของ `src_id` ก่อน แล้ววนตัวอื่น |
| B7 | ทำแล้ว | replica ไม่ค้างเมื่อ `exec_cb` คืน -1; `BYZ_REQ_HEADER_SIZE` ใน public header (มี static assert); example ตอบ `status=-1` แทนคืน -1 |

## 2. ข้อตกลงในการทำงาน

- **Branch:** แตกจาก `devlope/v0.2.9` ใช้ชื่อรูปแบบ `fix/develop/<version>` เช่น `fix/develop/v0.2.9` (ยังไม่ได้สร้าง)
- **Build และตรวจ:** ทำตาม `AGENT.md` หัวข้อ "After making changes" ต้อง build ทั้ง `examples/counter` และ `examples/simple_wallet` เพราะงาน A1, B1, B2 และ B7 กระทบทั้งสองตัวอย่าง
- **ห้ามเปิด timestamp anti-replay กลับมา** เหตุผลอยู่ใน `AGENT.md` หัวข้อ "Anti-replay design"
- **ห้ามเปลี่ยน wire format ของ message** ในรอบนี้
- **หนึ่งงานต่อหนึ่ง commit** และถ้าการแก้มีผลกับ variant เดียว ให้ระบุใน commit message กับ README ของ example
- **ห้าม commit SSID และรหัสผ่าน Wi-Fi** (ดู A7)

## 3. ตารางสรุปงาน

P0 = ทำก่อนการวัดรอบถัดไป, P1 = ต้องมีก่อนสรุปผลเทียบ, P2 = ทางเลือก ขนาดเป็นประมาณการคร่าว ๆ: S ไม่เกินครึ่งวัน, M 1–2 วัน, L หลายวัน

| ID | งาน | ความสำคัญ | ขนาด | ไฟล์หลัก |
|---|---|---|---|---|
| A1 | ESP-NOW ใช้ station interface แทน softAP | P1 | M | `main.c` ทั้งสอง example, `tbft_transport_espnow.c`, `Kconfig.projbuild` |
| A2 | UDP: ตั้ง `listen_interval`, เพิ่ม baseline `PS_NONE`, boot banner | P0 | S | `examples/counter/main/main.c`, `Kconfig.projbuild` |
| A3 | timeout ใน config สองไฟล์ให้เท่ากัน และแก้ `gen_configs.sh` | P0 | S | `gen_configs.sh`, `spiffs_image/*.txt` |
| A4 | คิวรับของ UDP ให้เท่ากับ ESP-NOW | P0 | S | `sdkconfig.defaults` |
| A5 | client ส่งตามคาบคงที่ และสรุปผลต่อ run | P1 | S | `examples/counter/main/main.c` |
| A6 | log level เท่ากันทุก variant และลด log บน hot path | P0 | S | `tbft_transport_espnow.c`, `tbft_replica.c` |
| A7 | จัดการ variant ด้วย overlay และ build dir แยก | P0 | S | `sdkconfig.*`, README, `.gitignore`, `flash_replicas.fish` |
| A8 | ตัวนับสถิติ | P1 | M | `src/*`, `include/esp-tinybft.h` |
| A9 | (ทางเลือก) ESP-NOW รับแบบ duty-cycle | P2 | L | `main.c`, `tbft_transport_espnow.c` |
| A10 | (ทางเลือก) PHY rate ของ ESP-NOW เป็นพารามิเตอร์ | P2 | S | `tbft_transport_espnow.c` |
| A11 | (ทางเลือก) CPU power management | P2 | L | `tbft_replica.c`, transport ทั้งสอง |
| B1 | ESP-NOW: รอ send callback และ retry เมื่อส่งไม่ถึง | P1 | M | `tbft_transport_espnow.c` |
| B2 | ESP-NOW: ลดงานใน recv callback | P1 | M | `tbft_transport_espnow.c` |
| B3 | delay ที่ต่ำกว่า 1 tick (รวม client busy-wait) | P0 | S | `tbft_libbyz.c`, `tbft_transport_udp.c` |
| B4 | UDP: retry เมื่อ `ENOMEM` | P1 | S | `tbft_transport_udp.c` |
| B5 | UDP: ตัวตนของ node และความล้มเหลวที่ต้องมองเห็น | P0 | S–M | `examples/counter/main/main.c` |
| B6 | client: ระบุผู้ส่ง reply จาก `src_id` | P2 | S | `tbft_libbyz.c` |
| B7 | `exec_cb` คืน -1 แล้ว replica ค้าง | P2 | S | `tbft_replica.c`, `main.c` ทั้งสอง example |

## 4. งานกลุ่ม A: ความแฟร์ของการทดลอง

### A1. ESP-NOW ใช้ station interface แทน softAP

**ปัญหา**

- `wifi_init()` ฝั่ง ESP-NOW ตั้ง `WIFI_MODE_AP` ช่อง 1 แบบ open ชื่อ `tinybft` (`examples/counter/main/main.c:117-134` และ `examples/simple_wallet/main/main.c:131`)
- transport ลงทะเบียน peer บน `WIFI_IF_AP` ด้วย MAC = base MAC + 1 (`src/tbft_transport_espnow.c:130-145`, `:698`)
- softAP เปิด RF ตลอด, ส่ง beacon ทุกประมาณ 100 ms และตอบ probe request ของอุปกรณ์รอบข้าง
- เอกสาร ESP-IDF v6.1 ระบุว่า modem-sleep ใช้ได้เฉพาะ station (`docs/en/api-guides/wifi-driver/wifi-performance-and-power-save.rst`) และ ESP-NOW "Sleep is supported only when configured as station" (`docs/en/api-reference/network/esp_now.rst`)

**วิธีแก้**

1. เพิ่ม Kconfig choice ใน component เช่น `TBFT_ESPNOW_IF_STA` (ค่าเริ่มต้น) กับ `TBFT_ESPNOW_IF_AP` (แบบเดิม)
2. เมื่อเป็น STA ให้ `tbft_transport_set_peer()` ใช้ MAC จาก config ตรง ๆ และ `ifidx = WIFI_IF_STA` ส่วน `find_node_by_mac()` (`:527-542`) เทียบ source MAC ตรง ๆ โดยไม่เรียก `get_ap_mac()` / `get_base_mac()`
3. แก้ `wifi_init()` ของทั้งสอง example ตามแบบ `examples/wifi/espnow` ของ ESP-IDF และเพิ่ม Kconfig `EXAMPLE_ESPNOW_CHANNEL`
4. อัปเดตหัวข้อ "Host app responsibilities (ESP-NOW)" ใน `AGENT.md`, `CLAUDE.md`, `GEMINI.md` และ README ของ example

```c
/* ESP-NOW, station interface, RF เปิดตลอด */
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
ESP_ERROR_CHECK(esp_wifi_init(&cfg));
ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
ESP_ERROR_CHECK(esp_wifi_start());
ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_EXAMPLE_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
ESP_ERROR_CHECK(esp_now_init());
```

**ข้อควรระวัง**

- ทุกบอร์ดต้องใช้ interface แบบเดียวกัน โหมด AP กับ STA คุยกันไม่ได้เพราะ MAC ปลายทางต่างกัน 1 จึงต้อง flash ใหม่ทั้ง 8 บอร์ด
- ไฟล์ config ไม่ต้องแก้ เพราะ station MAC ของ ESP32-C3 คือ base (eFuse) MAC ที่ใส่ไว้อยู่แล้ว

**เกณฑ์ตรวจรับ**

- boot log แสดง mode เป็น STA และ channel ที่ตั้ง
- scan Wi-Fi ไม่พบ SSID `tinybft`
- client ได้ reply สำเร็จ 100 request ติดต่อกัน ทั้ง `counter` และ `simple_wallet`

### A2. UDP: ตั้ง `listen_interval`, เพิ่ม baseline `PS_NONE`, boot banner

**ปัญหา**

- `wifi_cfg` ไม่ได้ตั้ง `listen_interval` (`examples/counter/main/main.c:88-91`) จึงเป็นค่าเริ่มต้น 3 beacon ถ้า router ใช้ DTIM=3 โหมด `PS_MIN` กับ `PS_MAX` จะทำงานเหมือนกัน
- ชุดทดลองไม่มี UDP `PS_NONE` ซึ่งเป็นคู่เทียบตรงกับ ESP-NOW ที่เปิด RF ตลอด
- help text ของ `EXAMPLE_WIFI_MODEM_PS` บอกว่าค่าเริ่มต้นคือ `MIN_MODEM` แต่ค่าจริงคือ `PS_NONE` (`examples/counter/main/Kconfig.projbuild:39-45`)
- ไม่มีหลักฐานใน log ว่าแต่ละบอร์ดรัน variant ไหน นอกจากบรรทัด "WiFi modem-sleep: …" ระดับ INFO

**วิธีแก้**

1. เพิ่ม Kconfig `EXAMPLE_WIFI_LISTEN_INTERVAL` (ค่าเริ่มต้น 3) แล้วใส่ใน `wifi_cfg.sta.listen_interval`
2. เพิ่ม variant `udp_psnone` ในชุด build (ดู A7)
3. พิมพ์ banner ตอน boot ด้วย `ESP_LOGW` หรือ `printf` เพื่อให้เห็นแม้ลด log level เนื้อหา: role, transport, interface, ค่า PS จาก `esp_wifi_get_ps()`, `listen_interval`, channel จาก `esp_wifi_get_channel()`, คาบ request และ version จาก `esp_app_get_description()`
4. แก้ help text ให้ตรงกับค่าเริ่มต้นจริง

**หมายเหตุ**

- DTIM ของ router ต้องจดจากหน้าตั้งค่า router ผู้รีวิวไม่พบ field นี้ใน `wifi_ap_record_t`
- เอกสาร ESP-IDF เตือนว่า `PS_MAX` อาจทำให้ข้อมูล broadcast หาย ซึ่งรวมถึง ARP request ถ้าพบ node ติดต่อกันไม่ได้ในช่วงแรก ให้เพิ่มเวลา warm-up ก่อนเริ่มวัด

**เกณฑ์ตรวจรับ:** banner ครบทุกบอร์ดและตรงกับ variant ที่ตั้งใจ

### A3. timeout ใน config สองไฟล์ให้เท่ากัน และแก้ `gen_configs.sh`

**ปัญหา**

| ค่า | `config_udp.txt` | `config_espnow.txt` | `gen_configs.sh` (UDP) |
|---|---|---|---|
| auth timeout (บรรทัด 3) | 3000 ms | 60000 ms | 3000 ms |
| view-change timeout (บรรทัด 14) | 30000 ms | 60000 ms | 2000 ms (`:55`) |
| status timeout | 5000 ms | 5000 ms | 5000 ms |
| recovery timeout | 10000 ms | 10000 ms | 10000 ms |

- view-change timeout ต่างกัน 2 เท่า run ที่มีข้อความหายจะฟื้นตัวไม่เหมือนกัน
- ค่าใน `config_udp.txt` ไม่ตรงกับที่ `gen_configs.sh` สร้าง ถ้ารัน script ซ้ำ view-change timeout จะกลายเป็น 2000 ms และ RSA key ทุกตัวจะถูกสร้างใหม่
- auth timeout ถูก parse แต่ไม่ถูกใช้ (`src/tbft_node.c:74` สร้าง timer โดยไม่มี callback และไม่เคย start)

**วิธีแก้**

1. ใน `gen_configs.sh` ประกาศตัวแปรชุดเดียว (`VC_TIMEOUT_MS`, `STATUS_TIMEOUT_MS`, `RECOVERY_TIMEOUT_MS`, `AUTH_TIMEOUT_MS`) แล้วใช้กับทั้งสองไฟล์
2. แยกการสร้าง key ออกจากการสร้าง config เช่น ข้ามการสร้าง key เมื่อมีไฟล์อยู่แล้ว หรือเพิ่ม flag `--keys`
3. ให้เจ้าของงานวิจัยเลือกค่า view-change timeout ค่าเดียว ค่าที่ใช้ต้องมากกว่า latency สูงสุดของ variant ที่ช้าที่สุดหลายเท่า

**เกณฑ์ตรวจรับ:** `diff` ของสองไฟล์ต่างกันเฉพาะบรรทัด multicast และบรรทัด node และการรัน script ซ้ำไม่เปลี่ยน key

### A4. คิวรับของ UDP ให้เท่ากับ ESP-NOW

**ปัญหา**

- UDP socket ใช้ `CONFIG_LWIP_UDP_RECVMBOX_SIZE=6` (ค่าเริ่มต้นของ IDF) เมื่อเต็ม lwIP ทิ้ง datagram โดยไม่มี log หรือสถิติ (`recv_udp()` ใน `components/lwip/lwip/src/api/api_msg.c`)
- ESP-NOW มีคิวรับ 16 ข้อความ (`MSG_QUEUE_DEPTH`, `src/tbft_transport_espnow.c:46`)
- ที่ n=7 แต่ละเฟสมี 6 ข้อความเข้าเกือบพร้อมกัน replica loop รับได้ไม่เกิน 1 ข้อความต่อ tick 10 ms (`src/tbft_replica.c:380-385`) และหยุดรับระหว่างทำ RSA
- ในโหมด PS router ปล่อยเฟรมที่ค้างไว้เป็นชุดเมื่อ station ตื่น จึงเสี่ยงล้นมากขึ้น **(อนุมาน)**

**วิธีแก้:** เพิ่ม `CONFIG_LWIP_UDP_RECVMBOX_SIZE=16` ใน `sdkconfig.defaults` ถ้าภายหลังเปลี่ยน `MSG_QUEUE_DEPTH` ให้เปลี่ยนค่านี้ตาม (แต่ละ entry ของคิว ESP-NOW ใช้ heap ราว 2 KB)

**หมายเหตุ:** ไม่ต้องแก้ `CONFIG_LWIP_IP_REASS_MAX_PBUFS` (=10) New_key จาก 6 peer ใช้สูงสุดราว 7 pbuf **(อนุมาน)**

**เกณฑ์ตรวจรับ:** ตัวนับ RX ต่อชนิดข้อความ (A8) ตรงกับค่าคาดหมายในภาคผนวกเมื่อรัน UDP `PS_MAX` 100 request

### A5. client ส่งตามคาบคงที่ และสรุปผลต่อ run

**ปัญหา**

- client หน่วง `CONFIG_EXAMPLE_CLIENT_REQUEST_INTERVAL_MS` หลังได้ reply (`examples/counter/main/main.c:233-241`) คาบจริงจึงเท่ากับ latency + interval และยาวเป็น 20 s + interval เมื่อ timeout
- จำนวน request ในช่วงวัดจึงต่างกันในแต่ละ variant
- ค่าเริ่มต้นใน Kconfig คือ 30000 ms แต่ `sdkconfig` ล่าสุดใช้ 10000 ms และก่อนหน้านั้น 2000 ms

**วิธีแก้**

1. ส่งตามเวลาสัมบูรณ์ ถ้าพลาดคาบให้ข้ามไปคาบถัดไปและนับเป็น request ที่ล้มเหลว
2. จับเวลาแต่ละ request และสรุปทุก N request: จำนวนสำเร็จ, ล้มเหลว, latency ต่ำสุด/เฉลี่ย/สูงสุด
3. กำหนดคาบเดียวกันทุก variant ผ่าน overlay (A7)

```c
const TickType_t period = pdMS_TO_TICKS(CONFIG_EXAMPLE_CLIENT_REQUEST_INTERVAL_MS);
TickType_t next = xTaskGetTickCount();
for (;;) {
    int64_t t0 = esp_timer_get_time();
    int rc = Byz_invoke(&req, &rep, false);
    int64_t latency_ms = (esp_timer_get_time() - t0) / 1000;
    /* อัปเดต n_ok / n_fail / latency min, avg, max แล้วพิมพ์สรุปทุก N request */

    next += period;
    while ((int32_t)(next - xTaskGetTickCount()) <= 0) {
        next += period;               /* พลาดคาบ: ข้าม และนับเป็น missed slot */
    }
    /* หลับเป็นช่วงไม่เกิน 1 s จนถึง next และ reset task WDT ทุกช่วง */
}
```

**เกณฑ์ตรวจรับ:** ช่วงห่างระหว่าง request ที่ log ได้คลาดจากคาบไม่เกิน 1 tick เมื่อไม่มี timeout

### A6. log level เท่ากันทุก variant และลด log บน hot path

**ปัญหา**

- ESP-NOW transport มี `ESP_LOGI` ทุกเฟรมที่ส่งและรับ (`src/tbft_transport_espnow.c:357`, `:477`, `:495`, `:516`) บรรทัด 357 อยู่ใน Wi-Fi task ส่วน UDP transport ไม่มี log ต่อข้อความเลย
- console UART เขียนแบบวนรอ TX FIFO (`uart_tx_char()` ใน `components/esp_driver_uart/src/uart_vfs.c`) log จึงกิน CPU และ block task ที่เรียก
- `src/tbft_replica.c:739`, `:975`, `:2065` เป็น debug dump ระดับ INFO
- `src/tbft_replica.c:2075` และ `:2784` พิมพ์ 4 byte แรกของ HMAC session key

**วิธีแก้**

1. เปลี่ยน log ทั้งหมดข้างต้นเป็น `ESP_LOGD` หรือแทนด้วยตัวนับ (A8) และลบการพิมพ์ key
2. สร้าง overlay `sdkconfig.measure` ที่ตั้ง `CONFIG_LOG_DEFAULT_LEVEL_WARN=y` ใช้กับทุก variant ตอนวัด
3. บรรทัดสรุปสถิติ (A5, A8) ต้องพิมพ์ด้วยระดับที่ยังมองเห็น

**เกณฑ์ตรวจรับ:** ระหว่าง run ปกติ ทุก variant พิมพ์เฉพาะ banner และบรรทัดสถิติ

### A7. จัดการ variant ด้วย overlay และ build dir แยก

**ปัญหา**

- ทุก variant และทุก role ใช้ `sdkconfig` ไฟล์เดียวและ build dir เดียว โดยแก้ค่าด้วยมือ
- ตัวเลือก PS, SSID และรหัสผ่าน `depends on TBFT_TRANSPORT_UDP` (`examples/counter/main/Kconfig.projbuild:23-53`) เมื่อสลับ transport ไปมา ค่าเหล่านี้เสี่ยงกลับเป็นค่าเริ่มต้น (`PS_NONE`)
- `sdkconfig.defaults` บรรทัด 1, 2 และ 15 กำหนด role, PS และ transport ไว้ตายตัว

**วิธีแก้**

1. ย้ายบรรทัดที่เลือก role, PS และ transport ออกจาก `sdkconfig.defaults` ให้เหลือเฉพาะค่าร่วม
2. สร้าง overlay ต่อไปนี้ใน `examples/counter/`

| ไฟล์ | เนื้อหาหลัก |
|---|---|
| `sdkconfig.variant.espnow` | `CONFIG_TBFT_TRANSPORT_ESPNOW=y` |
| `sdkconfig.variant.udp_psnone` | `CONFIG_TBFT_TRANSPORT_UDP=y`, `CONFIG_EXAMPLE_WIFI_PS_NONE=y` |
| `sdkconfig.variant.udp_psmin` | `CONFIG_TBFT_TRANSPORT_UDP=y`, `CONFIG_EXAMPLE_WIFI_PS_MIN_MODEM=y` |
| `sdkconfig.variant.udp_psmax` | `CONFIG_TBFT_TRANSPORT_UDP=y`, `CONFIG_EXAMPLE_WIFI_PS_MAX_MODEM=y`, `CONFIG_EXAMPLE_WIFI_LISTEN_INTERVAL=3` |
| `sdkconfig.role.replica` | `CONFIG_EXAMPLE_ROLE_REPLICA=y` |
| `sdkconfig.role.client` | `CONFIG_EXAMPLE_ROLE_CLIENT=y`, `CONFIG_EXAMPLE_CLIENT_REQUEST_INTERVAL_MS=<คาบที่ตกลง>` |
| `sdkconfig.measure` | `CONFIG_LOG_DEFAULT_LEVEL_WARN=y`, `CONFIG_TBFT_PRINT_STATS=y` |
| `sdkconfig.wifi.local` | SSID และรหัสผ่าน (ใส่ใน `.gitignore`) |

3. build แต่ละชุดใน build dir ของตัวเอง

```bash
idf.py -B build/udp_psmin_replica -D SDKCONFIG=build/udp_psmin_replica/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.variant.udp_psmin;sdkconfig.role.replica;sdkconfig.measure;sdkconfig.wifi.local" build
```

4. แก้ `flash_replicas.fish` ให้รับ build dir เป็น argument และเขียนตาราง variant กับคำสั่ง build ลง README ของ example

**หมายเหตุ:** `SDKCONFIG_DEFAULTS` มีผลเฉพาะตอนสร้าง `sdkconfig` ใหม่ ไฟล์ที่อยู่ท้ายรายการจะทับค่าของไฟล์ก่อนหน้า (`docs/en/api-guides/build-system.rst` ของ ESP-IDF)

**เกณฑ์ตรวจรับ:** ลบ `build/` แล้ว build ครบทุกชุดได้จากคำสั่งใน README โดยไม่เปิด `menuconfig`

### A8. ตัวนับสถิติ

**ปัญหา:** `CONFIG_TBFT_PRINT_STATS` และ `Byz_print_stats()` เป็นเพียง placeholder (`Kconfig.projbuild:135`, `src/tbft_libbyz.c:996-1012`) จึงไม่มีตัวเลขสำหรับ normalize พลังงานต่อ request หรือคัด run ที่ผิดปกติ

**วิธีแก้**

1. เพิ่มตัวนับภายใต้ `CONFIG_TBFT_PRINT_STATS` ดังนี้

| กลุ่ม | ตัวนับ |
|---|---|
| transport | ข้อความและ byte ที่ส่ง/รับ แยกตาม tag, ส่งล้มเหลว, คิวส่งเต็ม, คิวรับเต็ม, reassembly timeout |
| protocol | request ที่ execute, commit ที่ส่งซ้ำ, view-change ที่ส่ง, new-view ที่ติดตั้ง, view ปัจจุบัน, state fetch ที่เริ่ม |
| client | request สำเร็จ, ล้มเหลว, latency ต่ำสุด/เฉลี่ย/สูงสุด |

2. พิมพ์หนึ่งบรรทัดทุก 60 s ขึ้นต้นด้วย `TBFT_STATS` ในรูป `key=value` เพื่อให้ parse ได้ และเรียกได้จาก replica loop โดยตรงเพราะ `Byz_replica_run()` ไม่คืนค่า

**เกณฑ์ตรวจรับ:** ใน run ที่ไม่มี loss ตัวนับต่อ request ตรงกับตารางในภาคผนวก

### A9. (ทางเลือก) ESP-NOW รับแบบ duty-cycle

ทำได้หลัง A1 และ B1 เท่านั้น ใช้ `esp_now_set_wake_window()` และ `esp_wifi_connectionless_module_set_wake_interval()` หลัง `esp_now_init()` ตามแบบ `examples/wifi/espnow` ของ ESP-IDF (ค่าเริ่มต้นของตัวอย่างคือ window 50 ms, interval 100 ms) ใช้ได้เฉพาะ station และต้องมี `CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE=y` ซึ่งเปิดอยู่แล้ว ตัวอย่างไม่ได้เรียก `esp_wifi_set_ps()` เพราะค่าเริ่มต้นของ driver คือ `WIFI_PS_MIN_MODEM` อยู่แล้ว (`esp_wifi.h`)

**ข้อจำกัดที่ต้องออกแบบก่อน:** ผู้ส่งไม่รู้ว่า window ของผู้รับเปิดเมื่อไร เพราะไม่มี AP ร่วมให้ sync โอกาสส่งสำเร็จต่อครั้งจึงประมาณ window/interval **(อนุมาน)** ต้องมี retry ที่กินเวลาอย่างน้อยหนึ่ง interval และต้องทบทวน timeout ของ protocol

**การตัดสินใจ (2026-11-13):** รวม variant นี้ในชุดทดลอง เพราะเป็น mode ประหยัดพลังงานของ ESP-NOW เอง (modem sleep: ปิด RF นอก window ส่วน CPU ยังทำงาน) ไม่ใช่ light sleep

**สิ่งที่ทำ (ตรวจกับเอกสารและโค้ดของ Espressif แล้ว):**

- `main.c` เรียก API ทั้งสองตัวหลัง `esp_now_init()` ตามลำดับเดียวกับ `examples/wifi/espnow` และ `esp_wifi.h` แนะนำให้ interval เป็นพหุคูณของ 100 ms จุด sweep จึงใช้ 100 และ 200 ms
- คู่มือ Wi-Fi หัวข้อ "Connectionless Modules Power-saving" ระบุว่าส่งได้ทุกเวลาโดยไม่ต้องตั้งค่าเพิ่ม รับได้เฉพาะใน window และถ้าไม่เรียก API ทั้งสอง window จะเป็นค่าสูงสุด RF จึงเปิดตลอด variant `espnow` ใช้แบบนี้และตั้ง `WIFI_PS_NONE` เหมือน bulb ใน `coin_cell_demo` ของ repo esp-now
- ESP-IDF ไม่ sync window ให้ ผู้ส่งจึง retry ทุก `TBFT_ESPNOW_TX_RETRY_DELAY_MS` (น้อยกว่า window) จนได้ ACK หรือครบ `TBFT_ESPNOW_TX_RETRY_SPAN_MS` (sweep ใช้ interval + 20 ms) หลักนี้ตรงกับ application note ของ coin-cell button (`examples/coin_cell_demo/switch/docs/button_in_matter_bridging.md` ใน repo esp-now) ซึ่งใช้ bridge จาก `esp-now_bridge_light` ของ esp-matter ที่ตื่น 110 ms ทุก 200 ms ส่วนปุ่มส่งห่างกัน 50 ms รวม 3 ครั้ง เพื่อให้ครอบช่วง 90 ms ที่ bridge หลับ ข้อต่างคือปุ่มใช้ light sleep ระหว่างแต่ละครั้งและ bridge ต่อ AP อยู่ ส่วนระบบนี้รอโดยที่ CPU ยังทำงานและไม่มี AP
- วิธีที่เอกสาร ESP-IDF ให้ใช้เพื่อให้ window ตรงกัน คือต่อ AP เดียวกันแล้วส่งใน event `WIFI_EVENT_CONNECTIONLESS_MODULE_WAKE_INTERVAL_START` ต้องมี AP จึงไม่ได้ใช้
- ข้อสังเกต: คู่มือ Wi-Fi ของ v6.1 อ้างชื่อ `esp_wifi_set_connectionless_interval` ซึ่งไม่มีใน header ฟังก์ชันที่มีจริงคือ `esp_wifi_connectionless_module_set_wake_interval`

### A10. (ทางเลือก) PHY rate ของ ESP-NOW เป็นพารามิเตอร์

ESP-NOW ส่งที่ 1 Mbps เป็นค่าเริ่มต้น ส่วน UDP ผ่าน router ใช้ rate adaptation ของ 802.11n เวลาบนอากาศต่อเฟรมจึงต่างกันมาก (ดูภาคผนวก) เพิ่ม Kconfig สำหรับ rate แล้วเรียก `esp_now_set_peer_rate_config()` หลัง `esp_now_add_peer()` ค่าเริ่มต้นให้คง 1 Mbps และระบุค่าที่ใช้ในรายงานเสมอ

### A11. (ทางเลือก) CPU power management

**ปัญหา:** `CONFIG_PM_ENABLE` ปิดอยู่ และโค้ดใช้ polling จึงเข้า light-sleep ไม่ได้แม้เปิด PM

- replica loop รอ event 10 ms แล้ว delay อีก 10 ms เมื่อไม่มีข้อความ (`src/tbft_replica.c:380-385`, `:456`)
- ESP-NOW send task ตื่นทุก 50 ms (`SEND_TASK_RECV_TIMEOUT_MS`)

**วิธีแก้โดยสรุป:** เปิด `CONFIG_PM_ENABLE` และ `CONFIG_FREERTOS_USE_TICKLESS_IDLE`, เรียก `esp_pm_configure()` ตามแบบ `examples/wifi/power_save` และเปลี่ยน loop ให้ block จนมี event จริง เช่น ให้ transport ตั้ง event bit เมื่อมีข้อความเข้า

**การตัดสินใจ (2026-11-13):** ไม่ใช้ในการเปรียบเทียบ best-vs-best เพราะเจ้าของงานวิจัยกำหนดให้เทียบเฉพาะ mode ของวิทยุ `sdkconfig.pm` เก็บไว้สำหรับการทดลองแยก

**ผลต่อรายงาน:** ทุกจุดใน best-vs-best (`PS_MIN`, `PS_MAX` และ `espnow_ps`) เป็นการประหยัดเฉพาะส่วน RF โดย CPU ทำงานที่ 160 MHz ตลอด ต้องระบุในรายงาน

## 5. งานกลุ่ม B: ความเสถียรของ connection

### B1. ESP-NOW: รอ send callback และ retry เมื่อส่งไม่ถึง

**ปัญหา**

- `espnow_send_cb()` แค่ log เมื่อส่งไม่สำเร็จ (`src/tbft_transport_espnow.c:285-289`)
- retry loop ใน `espnow_do_send()` (`:369-440`) ดักเฉพาะกรณี `esp_now_send()` คืน error ตอน enqueue ไม่ได้ดักผลการส่งจริง
- send task log ว่า "OK" ก่อนรู้ผลจริง (`:495`, `:516`)
- ข้อความ 2 fragment (New_key 1840 B) ถ้า fragment ใดหาย ทั้งข้อความใช้ไม่ได้ แต่โค้ดยังส่ง fragment ที่เหลือ
- จังหวะส่งใช้ delay คงที่ 20 ms ต่อเฟรม, 20 ms ต่อ peer และ 50 ms ท้าย broadcast (`:44`, `:498`, `:506`) broadcast หนึ่งครั้งจึงใช้ราว 290 ms
- เอกสาร ESP-IDF แนะนำให้ส่งเฟรมถัดไปหลัง send callback ของเฟรมก่อนหน้ากลับมาแล้ว

**วิธีแก้**

1. เพิ่ม binary semaphore และตัวแปรสถานะใน `tbft_espnow_t` ให้ callback เก็บผลแล้ว give semaphore
2. ใน `espnow_do_send()` รอ semaphore หลัง `esp_now_send()` ทุกเฟรม ถ้าผลเป็น `ESP_NOW_SEND_FAIL` ให้ retry แบบจำกัดจำนวน
3. เมื่อ fragment ใดล้มเหลวหลัง retry ครบ ให้หยุดส่ง fragment ที่เหลือและคืน error
4. ลบ delay คงที่ทั้งสามจุด และ log ผลจริง (หรือนับใน A8)

```c
static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    tbft_espnow_t *enow = g_espnow_ctx;
    if (!enow) return;
    enow->tx_status = status;
    xSemaphoreGive(enow->tx_done);          /* callback รันใน Wi-Fi task ไม่ใช่ ISR */
}

static bool espnow_tx_frame(tbft_espnow_t *enow, const uint8_t *mac,
                            const uint8_t *pkt, size_t len)
{
    for (int attempt = 0; attempt < ESPNOW_TX_MAX_ATTEMPTS; attempt++) {
        xSemaphoreTake(enow->tx_done, 0);   /* ล้างผลค้างจากเฟรมก่อน */
        if (esp_now_send(mac, pkt, len) == ESP_OK &&
            xSemaphoreTake(enow->tx_done, pdMS_TO_TICKS(ESPNOW_TX_CB_TIMEOUT_MS)) == pdTRUE &&
            enow->tx_status == ESP_NOW_SEND_SUCCESS) {
            return true;
        }
        vTaskDelay(1);                      /* backoff 1 tick */
    }
    return false;
}
```

**ข้อควรระวัง:** มีเพียง send task ที่เรียก `esp_now_send()` จึงมีเฟรมค้างส่งได้ทีละหนึ่งเฟรม ต้องคงเงื่อนไขนี้ไว้

**เกณฑ์ตรวจรับ**

- broadcast ถึง 6 peer เสร็จภายในราว 60 ms ในสภาพปกติ (ประมาณการ ให้ปรับตามผลวัดจริง)
- เมื่อปิดไฟ replica หนึ่งตัว ตัวที่เหลือนับการส่งล้มเหลวไปยัง node นั้น และ client ยังได้ reply

### B2. ESP-NOW: ลดงานใน recv callback

**ปัญหา** (`espnow_recv_cb()`, `src/tbft_transport_espnow.c:291-363`, รันใน Wi-Fi task)

- `xSemaphoreTake(enow->lock, pdMS_TO_TICKS(5))` ที่บรรทัด 310 เท่ากับ 0 tick ที่ `CONFIG_FREERTOS_HZ=100` ถ้า task อื่นถือ lock อยู่ เฟรมถูกทิ้งทันทีโดยไม่มี log หรือตัวนับ
- `recv_entry_t q_entry` ที่บรรทัด 335 ใช้ stack ของ Wi-Fi task ราว 2 KB และข้อมูลถูก copy สองรอบ
- `ESP_LOGI` ที่บรรทัด 357 ถูกเรียกทุกข้อความ
- เมื่อคิวรับเต็ม ข้อความถูกทิ้งโดยไม่มีตัวนับ (`:348-355`)

**วิธีแก้**

1. ให้ `enow->reasm[]` ถูกแตะจาก Wi-Fi task เท่านั้น โดยลบการเรียก `reasm_clear_stale()` ใน send task (`:452-459`, `:463-466`) และใน `tbft_transport_recv()` (`:738-745`) callback เรียกฟังก์ชันนี้เองทุกครั้งอยู่แล้ว (`:318`) จากนั้นเลิกใช้ `lock` ใน callback
2. ใช้ `static recv_entry_t` ที่ใช้เฉพาะใน callback แทนตัวแปรบน stack
3. เพิ่มทางลัดเมื่อ `frag_total == 1` ให้ copy ลง entry แล้วส่งเข้าคิวโดยไม่ผ่าน reassembly slot ข้อความทุกชนิดยกเว้น New_key เป็น fragment เดียว
4. ลบ `ESP_LOGI` และเพิ่มตัวนับคิวรับเต็ม (A8)

**เกณฑ์ตรวจรับ:** callback ไม่มีการรอ lock, ไม่มี log และไม่มีตัวแปรขนาดใหญ่บน stack และ run 30 นาทีไม่มีตัวนับ drop เพิ่ม

### B3. delay ที่ต่ำกว่า 1 tick

**ปัญหา:** ที่ `CONFIG_FREERTOS_HZ=100` ค่า `pdMS_TO_TICKS(x)` เมื่อ x < 10 เท่ากับ 0

| ตำแหน่ง | ผลที่เกิด |
|---|---|
| `src/tbft_libbyz.c:645`, `:685` | client วนรอ reply โดยไม่หลับ CPU ทำงาน 100% ได้นานถึง 20 s ต่อ request |
| `src/tbft_transport_udp.c:58` | retry ครั้งแรกไม่มี backoff |
| `src/tbft_transport_espnow.c:310` | แก้ใน B2 |
| `src/tbft_replica.c:357` | เป็นเพียง yield ไม่ต้องแก้ เพราะ loop block ที่ `xEventGroupWaitBits` อยู่แล้ว |

**วิธีแก้:** เพิ่ม macro กลาง เช่น `TBFT_MS_TO_TICKS_MIN1(ms)` ที่คืนอย่างน้อย 1 tick แล้วใช้ในสองตำแหน่งแรก

```c
#define TBFT_MS_TO_TICKS_MIN1(ms)  ((pdMS_TO_TICKS(ms) > 0) ? pdMS_TO_TICKS(ms) : 1)
```

**ผลต่อการวัด:** ถ้าวัดพลังงานที่บอร์ด client ช่วงรอ reply จะกินไฟมากกว่าปกติ และยาวตาม latency ของแต่ละ variant

**เกณฑ์ตรวจรับ:** ระหว่างรอ reply task ของ client อยู่ในสถานะ blocked เป็นส่วนใหญ่ ตรวจได้จาก run-time stats ของ FreeRTOS หรือจากกระแสของบอร์ด client ที่ลดลงในช่วงรอ

### B4. UDP: retry เมื่อ `ENOMEM`

**ปัญหา:** `udp_sendto_retry()` retry เฉพาะ `EAGAIN` / `EWOULDBLOCK` (`src/tbft_transport_udp.c:46-63`) แต่เมื่อ Wi-Fi TX buffer เต็ม ESP-IDF คืน `ENOMEM` (`components/esp_netif/lwip/netif/wlanif.c` แปลง `ESP_ERR_NO_MEM` เป็น `ERR_MEM` และ `components/lwip/lwip/src/api/err.c` แปลง `ERR_MEM` เป็น `ENOMEM`) กรณีที่ comment ในโค้ดอธิบายจึงไม่ถูก retry

**วิธีแก้:** ถือว่า `ENOMEM` และ `ENOBUFS` เป็น error ชั่วคราวด้วย, ใช้ backoff อย่างน้อย 1 tick (B3) และนับการส่งล้มเหลว (A8)

**เกณฑ์ตรวจรับ:** ตัวนับส่งล้มเหลวเป็นศูนย์ใน run ปกติ

### B5. UDP: ตัวตนของ node และความล้มเหลวที่ต้องมองเห็น

**ปัญหา**

- node หา id ของตัวเองจากการเทียบ IP ที่ได้จาก DHCP กับ `config_udp.txt` จึงต้องพึ่ง DHCP reservation ของ router
- ถ้า IP ไม่ตรง `Byz_detect_local_id()` คืน -1 แล้ว task ลบตัวเอง (`examples/counter/main/main.c:145-151`, `:181-187`) บอร์ดยังเปิดวิทยุแต่ไม่ทำงาน ค่าพลังงานที่วัดจากบอร์ดนั้นใช้ไม่ได้
- Wi-Fi event handler เรียก `esp_wifi_connect()` ทันทีทุกครั้งที่หลุดโดยไม่มี backoff และ `WIFI_FAIL_BIT` ไม่เคยถูก set (`:25-37`, `:106-116`)

**วิธีแก้**

- **ขั้นต่ำ:** เมื่อหา id ไม่ได้ ให้ log ระดับ ERROR ซ้ำทุกไม่กี่วินาที แล้ว `esp_restart()` หลังพยายามครบจำนวน และเขียนข้อกำหนด DHCP reservation ลง README
- **แนะนำ:** กำหนด static IP จาก MAC โดยอ่าน eFuse MAC, หา index ใน `config_espnow.txt`, ใช้ IP ของ index เดียวกันใน `config_udp.txt` แล้วเรียก `esp_netif_dhcpc_stop()` กับ `esp_netif_set_ip_info()` ก่อน connect (netmask และ gateway มาจาก Kconfig) วิธีนี้ใช้ firmware เดียวได้ทุกบอร์ด
- เพิ่ม backoff ใน event handler และลบโค้ดที่ไม่ถูกใช้

**เกณฑ์ตรวจรับ:** เปิดบอร์ดโดยที่ router ไม่มี reservation แล้วบอร์ดยังได้ id ถูกต้อง (แบบแนะนำ) หรือ restart พร้อม log ที่ชัดเจน (แบบขั้นต่ำ)

### B6. client: ระบุผู้ส่ง reply จาก `src_id`

**ปัญหา:** `Byz_recv_reply()` หาผู้ส่งโดยลอง verify ลายเซ็น RSA ด้วย public key ของ replica ทีละตัว สูงสุด 7 ครั้งต่อ reply (`src/tbft_libbyz.c:676-682`) และเรียก `tbft_node_recv()` โดยไม่ขอ `src_id` (`:643`)

**วิธีแก้:** ขอ `src_id` จาก transport แล้วลอง key ของ node นั้นก่อน ถ้าไม่ผ่านจึงลองตัวอื่นตามเดิม ลายเซ็นยังถูกตรวจทุกครั้ง

**เกณฑ์ตรวจรับ:** ในกรณีปกติมีการ verify หนึ่งครั้งต่อ reply

### B7. `exec_cb` คืน -1 แล้ว replica ค้าง

**ปัญหา**

- เมื่อ `exec_cb` คืนค่าไม่ใช่ 0 loop execute จะ `break` โดยไม่ขยับ `last_executed` (`src/tbft_replica.c:2321-2324`) replica จึงเรียก request เดิมซ้ำไปเรื่อย ๆ
- เอกสารของ API ระบุว่า -1 หมายถึงปฏิเสธและไม่ส่ง reply (`include/esp-tinybft.h:115`)
- `exec_cb` ของ counter คืน -1 เมื่อ `op` ไม่รู้จัก (`examples/counter/main/main.c:67-69`)
- ทั้งสอง example ใช้ค่าคงที่ 68 เพื่อข้าม header ของ request (`examples/counter/main/main.c:52-53`, `examples/simple_wallet/main/main.c:61-62`)

**วิธีแก้**

1. ใน `tbft_replica_execute_committed()` เมื่อ `exec_cb` ปฏิเสธ ให้ถือว่า seqno นั้น execute แล้ว ไม่ส่ง reply แล้วทำต่อ
2. ใน example ให้ตอบ reply ที่มี `status` เป็นค่า error แทนการคืน -1 เพื่อไม่ให้ client รอจน timeout
3. เพิ่มค่าคงที่ใน public header สำหรับ offset ของ command แทนเลข 68

**เกณฑ์ตรวจรับ:** ส่ง request ที่ `op` ไม่ถูกต้องหนึ่งครั้ง แล้ว request ปกติถัดไปยังสำเร็จ

## 6. ประเด็นระดับ protocol ที่ไม่แก้ในรอบนี้

ประเด็นเหล่านี้อยู่นอกขอบเขต connection แต่ทำให้ข้อความหายหนึ่งครั้งกลายเป็นงาน RSA และ broadcast ชุดใหญ่ run ที่เกิดเหตุการณ์เหล่านี้ต้องแยกรายงาน โดยใช้ตัวนับจาก A8

| ID | ประเด็น | ตำแหน่ง |
|---|---|---|
| C1 | view-change ซ้ำจาก replica ตัวเดียวทำให้ตัวอื่นขยับ view โดยไม่ต้องมี quorum | `src/tbft_replica.c:1185-1232` |
| C2 | Status ไม่มี authenticator แต่ทำให้ replica ขยับ view ตามได้ | `src/tbft_replica.c:1626-1698` |
| C3 | replica ข้าม seqno ที่ยังไม่ commit หรือไม่มี pre-prepare แล้วซ่อม state ด้วย fetch ภายหลัง | `src/tbft_replica.c:2237-2265` |
| C4 | ไม่มีการกัน request ที่ `rid` เก่ากว่าที่ execute ไปแล้ว จึง execute ซ้ำได้ | `src/tbft_replica.c:607-609`, `:643-668` |
| C5 | commit ถูกส่งซ้ำตามเงื่อนไขเวลา 500 ms ปริมาณข้อความจึงขึ้นกับ latency ของแต่ละ variant | `src/tbft_replica.c:1089-1093` |
| C6 | client ส่ง request ครั้งเดียวแล้วรอ ไม่มีการส่งซ้ำ | `src/tbft_libbyz.c:562-628`, `:782-787` |

**คำเตือนสำหรับ C6:** อย่าเพิ่มการส่งซ้ำฝั่ง client ก่อนแก้ C4 เพราะจะทำให้ request ถูก execute ซ้ำได้

## 7. ระเบียบวิธีการวัด

**ชุดทดลองที่แนะนำ**

| | RF เปิดตลอด | ประหยัดพลังงาน |
|---|---|---|
| ESP-NOW | station interface (A1) | wake window/interval ของ ESP-NOW (A9) |
| UDP | `PS_NONE` | `PS_MIN`, `PS_MAX` |

ไม่มีจุดใดใช้ CPU light sleep (A11 อยู่นอกการเปรียบเทียบ) ขั้นตอนเต็มของ best-vs-best อยู่ใน `examples/counter/README.md`

**เช็กลิสต์ต่อ run**

- [ ] ทุกบอร์ด (7 replica + 1 client) รัน variant เดียวกัน ตรวจจาก boot banner
- [ ] วัดบอร์ดที่มีบทบาทเดียวกันทุก variant (primary, backup หรือ client) และจด view ตอนเริ่มกับตอนจบ
- [ ] ตัดช่วง boot และ key exchange ออก เริ่มวัดหลัง request แรกสำเร็จ
- [ ] วัด baseline ตอนไม่มี client แยกต่างหาก แล้วรายงานพลังงานส่วนเพิ่มต่อ request ที่สำเร็จ
- [ ] บันทึกจำนวน request สำเร็จและล้มเหลว, latency และบรรทัด `TBFT_STATS`
- [ ] run ที่มี view-change, state fetch หรือ request ล้มเหลว ให้รายงานแยก
- [ ] ใช้ channel เดียวกันทั้ง ESP-NOW และ UDP และจด channel, beacon interval กับ DTIM ของ router
- [ ] ระบุในรายงานว่าพลังงานของ router ไม่ถูกนับในฝั่ง UDP
- [ ] ทำซ้ำอย่างน้อย 3 run ต่อ variant และรายงานค่าเฉลี่ยกับส่วนเบี่ยงเบน

## 8. ลำดับการทำงานที่แนะนำ

1. A7 ก่อน เพื่อให้ทุกงานถัดไป build และทดสอบได้ครบทุก variant
2. งาน P0 ที่เหลือ: A3, A4, A6, A2, B3, B5 แล้ววัดรอบตรวจสอบหนึ่งรอบ
3. ESP-NOW transport ตามลำดับ B1, B2, A1 (A1 ทดสอบง่ายขึ้นเมื่อ B1 รายงานผลการส่งจริงแล้ว)
4. A8, A5, B4 แล้วรันชุดทดลองตามข้อ 7
5. งาน P2 ตามที่เจ้าของงานวิจัยตัดสินใจ

## ภาคผนวก: ค่าอ้างอิง

**ขนาดข้อความบนสาย (n=7, คำนวณจาก struct ใน `src/tbft_message.h`)**

| ข้อความ | ขนาด (byte) |
|---|---|
| Request | 328 |
| Pre-prepare | 592 |
| Prepare, Commit | 264 |
| Checkpoint | 256 |
| Status | 56 |
| Reply | 312 |
| New_key | 1840 (2 fragment ทั้งสอง transport) |

**จำนวนข้อความที่คาดหมายต่อ 1 request เมื่อไม่มี loss (view 0, primary คือ node0)**

| บทบาท | ส่ง | รับ |
|---|---|---|
| backup | 14 (forward 1, Prepare 6, Commit 6, Reply 1) | 14 (Request 1, Pre-prepare 1, Prepare 6, Commit 6) |
| primary | 19 (Pre-prepare 6, Prepare 6, Commit 6, Reply 1) | 19 (Request 1, forward 6, Prepare 6, Commit 6) |
| client | 7 | 7 |

- ทุก 2 request แต่ละ replica ส่งและรับ Checkpoint เพิ่มอย่างละ 6
- ทุก 5 s แต่ละ replica ส่งและรับ Status อย่างละ 6
- ทุก 60 s เมื่อมี key ของ peer อย่างน้อย 4 ตัว แต่ละ replica ส่งและรับ New_key อย่างละ 6 (ถ้ายังไม่ถึงเกณฑ์จะส่งทุก 5 s, `src/tbft_replica.c:370-371`)

**เวลาส่งของ ESP-NOW ในโค้ดก่อนแก้ B1 (อนุมานจาก delay ในโค้ด)**

| การส่ง | เวลาโดยประมาณ |
|---|---|
| broadcast ข้อความ fragment เดียว | 6 × (20 + 20) + 50 = 290 ms |
| broadcast New_key | 6 × (2 × 20 + 20) + 50 = 410 ms |
| unicast | 20 + 10 = 30 ms |
| เวลาบนอากาศที่ 1 Mbps | ราว 2.7 ms (264 B), 5.4 ms (592 B), 12.6 ms (1470 B) |

**กระแสเฉลี่ยของ ESP32-C3 ขณะเชื่อมต่อ Wi-Fi โดยไม่มี traffic** (`docs/en/api-guides/sleep-current/esp32c3_summary.inc` ของ ESP-IDF v6.1)

| โหมด | DTIM1 | DTIM3 |
|---|---|---|
| Modem-sleep | 21.47 mA | 20.82 mA |
| Modem-sleep + DFS | 11.35 mA | 10.71 mA |
| Auto light-sleep | 1.4 mA | 0.62 mA |

**กระแสของชิป ESP32-C3** (ESP32-C3 Series Datasheet หัวข้อ 5.6, ค่า peak ที่ 3.3 V และ 25 °C, ตรวจเมื่อ 2026-11-13)

| สถานะ | กระแส |
|---|---|
| รับ 802.11b/g/n HT20 | 84 mA |
| ส่ง 802.11b 1 Mbps @21 dBm | 335 mA |
| ส่ง 802.11g 54 Mbps @19 dBm | 285 mA |
| ส่ง 802.11n HT20 MCS7 @18.5 dBm | 276 mA |
| modem sleep 160 MHz, CPU idle / ทำงาน (ปิด clock ของ peripheral ทั้งหมด) | 16 / 23 mA |
| light sleep | 130 µA |

ค่าระดับบอร์ดจะสูงกว่านี้ (regulator, USB bridge, LED)

**สิ่งที่รีวิวนี้ไม่ได้ครอบคลุม:** วงจรและเครื่องมือวัดพลังงาน, ความถูกต้องของ state transfer, การรีวิวความปลอดภัยทั้งระบบ และการทดสอบบนบอร์ดจริง
