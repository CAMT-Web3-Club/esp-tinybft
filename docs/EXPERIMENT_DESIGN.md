# การออกแบบการทดลอง counter: ทำไมใช้ preset และต่างจากการทดลองเดิมอย่างไร

| | |
|---|---|
| วันที่ | 2026-11-20 |
| เทียบระหว่าง | `devlope/v0.2.9` (commit `0a7b8ee`, 2026-08-24) กับ `fix/develop/v0.2.9` (38 commit ถัดมา) |
| รายละเอียดเพิ่ม | `docs/FIX_PLAN_COUNTER_ENERGY_FAIRNESS.md` (A, B, C, X), `docs/REVIEW_2026-11-15.md` (L, X, R), `examples/counter/README.md` |

เอกสารนี้อธิบายสามเรื่อง: (1) การทดลองเดิมวัดอะไรและทำไมผลเทียบกันไม่ได้, (2) บั๊กที่พบและแก้แล้ว และ (3) เหตุผลของ preset, โหมดที่เพิ่ม และ scenario ใหม่ รหัส A1, B3, C4, X8 ฯลฯ อ้างอิงเอกสารสองฉบับข้างบน

## 1. สรุป

- การทดลองเดิมเทียบ **UDP `PS_NONE` / `PS_MIN` / `PS_MAX`** กับ **ESP-NOW ค่าเริ่มต้น** โดยให้ client เว้นระยะ **2, 10 และ 40 s** แต่ระหว่างสองฝั่งมีตัวแปรที่ไม่เกี่ยวกับ transport ต่างกันหลายตัว (โหมดวิทยุ, timeout, คิวรับ, log, จังหวะส่ง) และมีบั๊กที่ทำให้ CPU ทำงาน 100 % ระหว่างรอ reply ผลพลังงานที่ได้จึงสะท้อนบั๊กและการตั้งค่า มากกว่าตัว transport
- branch `fix/develop/v0.2.9` แก้บั๊กด้านความถูกต้องของ PBFT, liveness, transport และการวัดผล และทำให้ทุกตัวแปรที่ไม่ใช่ตัวทดลองเท่ากันทุก config
- **Experiment preset** รวมการตั้งค่าของแต่ละโหมดไว้ในตัวเลือกเดียวใน menuconfig มี 17 preset ครอบคลุมทุกโหมดที่ใช้งานได้บนบอร์ด (ESP-NOW 9, UDP 8) และ **Experiment load** เลือกภาระงานได้ 2 แบบ (back-to-back และส่งทุก 10 s) รวมเป็น 34 จุดทดลอง
- **ผลที่วัดก่อน `fix/develop/v0.2.9` ใช้เทียบกับผลใหม่ไม่ได้** ต้องวัดใหม่ทั้งหมด

## 2. การทดลองเดิม (`devlope/v0.2.9`)

### 2.1 สิ่งที่วัด

| ฝั่ง | ตั้งค่าอย่างไร |
|---|---|
| UDP | `PS_NONE`, `PS_MIN`, `PS_MAX` เลือกใน menuconfig, ส่ง unicast ทีละ replica, หา id จาก IP ที่ได้จาก DHCP |
| ESP-NOW | ค่าเริ่มต้นตัวเดียว: interface **softAP** ช่อง 1 (SSID `tinybft`), rate ไม่ได้ตั้ง (1 Mbps), ไม่มี power saving |
| client | ส่ง request แล้ว**รอ interval หลังได้ reply** (2, 10 หรือ 40 s) |
| role | build แยก image ของ replica กับ client (ค่าเริ่มต้นใน `sdkconfig.defaults` คือ Replica) |

### 2.2 ทำไมผลเดิมเทียบกันไม่ได้

1. **โหมดวิทยุไม่เท่ากัน (A1)** ESP-NOW รันบน softAP ซึ่งเปิด RF ตลอด ส่ง beacon ทุกประมาณ 100 ms และตอบ probe request ของอุปกรณ์รอบข้าง ส่วน UDP PS ปิด RF เป็นช่วง ความต่างที่วัดได้จึงมาจากโหมดวิทยุมากกว่า transport นอกจากนี้ ESP-IDF ระบุว่า ESP-NOW หลับได้เฉพาะบน station เท่านั้น
2. **CPU ทำงาน 100 % ระหว่างรอ reply (B3)** ที่ tick 100 Hz `pdMS_TO_TICKS(x)` ของค่าที่ต่ำกว่า 10 ms ได้ 0 client จึงวนรอ reply โดยไม่หลับ นานถึง 20 s ต่อ request พลังงานของ client ส่วนนี้ไม่เกี่ยวกับ transport เลย
3. **ภาระงานต่างกันในแต่ละ config (A5)** client รอ interval *หลัง* ได้ reply คาบจริงจึงเท่ากับ latency + interval config ที่ช้ากว่าจะส่ง request น้อยกว่าในเวลาเท่ากัน และเมื่อ request ล้มเหลว คาบจะยาวเป็น 20 s + interval จำนวน request ต่อ run จึงไม่เท่ากันในแต่ละ config
4. **timeout ไม่เท่ากันและ menuconfig ไม่มีผล (A3, X2)** ค่าจริงมาจาก config file: ESP-NOW ใช้ view-change timeout 60 s และ auth timeout 60 s ส่วน UDP ใช้ 30 s และ 3 s การฟื้นตัวเมื่อข้อความหายจึงต่างกันสองเท่า ส่วนค่าใน menuconfig ไม่ถูกใช้เลย
5. **คิวรับไม่เท่ากัน (A4)** UDP ใช้คิวของ lwIP 6 ช่อง (ทิ้ง datagram เงียบๆ เมื่อเต็ม) ส่วน ESP-NOW ใช้ 16 ช่อง
6. **log ไม่เท่ากัน (A6)** ESP-NOW พิมพ์ log ระดับ INFO ทุกเฟรมที่รับส่ง และ console UART รอ FIFO แบบ busy-wait ส่วน UDP ไม่มี log ต่อข้อความ log ยังพิมพ์บางส่วนของ HMAC session key ออกมาด้วย
7. **`listen_interval` ไม่ได้ตั้ง (A2)** จึงใช้ค่าเริ่มต้น 3 beacon ถ้า router ตั้ง DTIM = 3 โหมด `PS_MIN` กับ `PS_MAX` จะทำงานเหมือนกัน
8. **ESP-NOW ส่งด้วย delay คงที่ (B1)** หน่วง 20 ms ต่อเฟรม 20 ms ต่อ peer และ 50 ms ท้าย broadcast broadcast หนึ่งครั้งจึงใช้ประมาณ 290 ms และไม่ได้ตรวจผลการส่งจริงจาก send callback
9. **ข้อความหายไม่มีใครรู้** ทั้งสอง transport ทิ้งข้อความโดยไม่มีตัวนับ และ log ไม่ระบุว่าบอร์ดรัน config ไหน จึงตัด run ที่มีปัญหาออกไม่ได้
10. **ข้อความที่ส่งซ้ำตามเวลา (C5)** replica ส่ง Commit ซ้ำทุก 500 ms แม้ไม่มีข้อความหาย จำนวนข้อความต่อ request จึงขึ้นกับว่า request ใช้เวลานานเท่าไร ไม่ได้ขึ้นกับ protocol

## 3. บั๊กที่พบและแก้แล้ว

### 3.1 ความถูกต้องของ PBFT (C1–C11)

| ID | ปัญหาเดิม | ผล |
|---|---|---|
| C1 | replica ตัวเดียวทำให้ตัวอื่นเปลี่ยน view ได้โดยไม่ต้องมี quorum | view change เกิดง่ายเกินไป |
| C2 | Status ไม่มี authenticator แต่ทำให้ผู้รับเปลี่ยน view ตามได้ | ปลอมข้อความเปลี่ยน view ได้ |
| C3 | replica ข้าม seqno ที่ยังไม่ commit | state ของ replica ต่างกันได้ |
| C4 | execute request ซ้ำได้ | counter นับเกิน |
| C5 | Commit ถูกส่งซ้ำทุก 500 ms | traffic เพิ่มตาม latency ไม่ใช่ตาม loss |
| C6 | client ส่ง request ครั้งเดียว | request ที่ข้อความหายครั้งเดียวล้มเหลวทันที |
| C7 | Prepare ของ primary ถูกนับใน quorum | primary ที่ผิดพลาดทำให้ replica ที่ถูกต้องสองตัว prepared คนละ request ที่ seqno เดียวกันได้ |
| C8–C11 | New_view ต้องมี View_change ครบ, fetch ส่ง state ผิดชุด, replica ที่บูตช้ารอ key ได้ถึง 60 s, digest ไม่คลุม non-det | replica ที่บูตช้าตามไม่ทัน หรือ fetch ได้ state ผิด |

ตอนนี้ replica execute ตามลำดับ seqno เคร่งครัด (exactly-once) client ส่ง request ซ้ำได้อย่างปลอดภัย และ request id ไม่ย้อนกลับเมื่อ client reboot

### 3.2 Liveness เมื่อมี loss หรือ replica เหลือพอดี 2f+1 (L1–L9)

พบโดย review หลายมุมและ simulator (`tests/sim`) เช่น View_change ส่งครั้งเดียวไม่ส่งซ้ำ (L1), fetch เล็ง checkpoint ที่ไม่มีใครส่งให้ได้จน cluster ค้างถาวร (L2), ซ่อมข้อความที่หายช้ากว่า view-change timeout (L5) และ ESP-NOW broadcast ช้าเมื่อมี peer ตาย (L8) แก้ทั้งหมดแล้ว และพิสูจน์บนบอร์ดด้วย loss 15 % ร่วมกับ reset primary สองตัว ระบบไปที่ view 2 แล้วให้บริการต่อได้

### 3.3 Transport

| ID | ปัญหาเดิม | ผลที่วัดได้หลังแก้ |
|---|---|---|
| B1, X8 | ESP-NOW ไม่รอ send callback และใช้ timeout 100 ms ที่สั้นเกินไป จึงส่งเฟรมซ้ำทั้งที่ driver ยังถือเฟรมเดิมอยู่ | heap ต่ำสุดเพิ่มจาก 4.4–40 KB เป็น 44–46 KB, การส่งล้มเหลวจาก 15–630 ครั้งต่อบอร์ดเหลือ 0, latency จาก 4.9 s เหลือ 2.0 s |
| B2 | recv callback ถือ lock, log ทุกเฟรม, ทิ้งเฟรมเงียบๆ | callback ทำงานน้อยลง และนับเฟรมที่ทิ้ง |
| B4 | UDP ไม่ retry เมื่อ `ENOMEM` (Wi-Fi TX buffer เต็ม) | retry และนับ |
| B5 | UDP หา id ไม่ได้แล้ว task หยุดเงียบๆ แต่วิทยุยังเปิดอยู่ ค่าพลังงานของบอร์ดนั้นใช้ไม่ได้ | log ERROR แล้ว reboot และมี option ตั้ง static IP จาก MAC |
| – | ESP-NOW ส่งเฟรมซ้ำเมื่อ ACK หาย ผู้รับได้ New_key หรือ Status ซ้ำ | ผู้รับทิ้งเฟรมซ้ำ (`rx_dup`) |

### 3.4 การวัดผล (X1–X7) และความทนทาน (R1–R11, B7)

- **X2:** timeout มาจาก menuconfig อย่างเดียว และ BANNER พิมพ์ค่าที่ใช้จริง
- **X3–X5:** ตัวนับนับตั้งแต่ request แรก, นับการส่งซ้ำและการทิ้งครบทุกเส้นทาง และ `analyze.py` ตัด run ที่มี view change, fetch หรือ reboot ได้อัตโนมัติ
- **R1:** `handle_fetch` อ่านเกินขอบเขต buffer ได้
- **B7:** `exec_cb` คืน -1 แล้ว replica เรียก request เดิมซ้ำไม่จบ
- **R3–R11:** ตรวจขนาด command, ความจุของ reply, NVS และอื่นๆ
- **หน่วยความจำ:** component เป็น static ทั้งหมด ไม่ใช้ heap ของตัวเอง

## 4. ทำไมใช้ preset

### 4.1 ปัญหาของการตั้งค่าแบบเดิม

การทดลองหนึ่งจุดต้องตั้งค่า 6–8 ตัวให้ตรงกันทุกบอร์ด ได้แก่ transport, PHY rate, interface, receive power saving, broadcast, multicast, attempts per frame, Wi-Fi PS, listen interval และ minimum active time ถ้าตั้งพลาดหนึ่งตัวในบอร์ดเดียว run นั้นใช้ไม่ได้และไม่มีใครรู้ บาง combination ก็ใช้งานไม่ได้จริงด้วย เช่น broadcast ร่วมกับ power saving (X6) หรือ PHY rate ที่ ACK หาย

### 4.2 สิ่งที่ preset ให้

1. **เลือกครั้งเดียวได้ทั้งชุด** เมนูอื่นจะเหลือแค่ตัวเลือกของ preset นั้น จึงตั้งผิดไม่ได้
2. **มีเฉพาะโหมดที่ใช้งานได้บนบอร์ด** rate 6/6.5/11/24/54 Mbps และ ESP-NOW broadcast ไม่มี preset เพราะ ACK หาย 73–100 % หรือค้างที่ seqno
3. **ตัวแปรที่ต้องเท่ากันถูกกำหนดเท่ากัน** attempts per frame = 3, multicast ปิด, broadcast ปิด, minimum active time = 50 ms ยกเว้น preset ที่ตั้งใจเปลี่ยนค่านั้น (`_t8`, `_mcast`, `_a20`)
4. **ตรวจย้อนได้** BANNER ของทุกบอร์ดพิมพ์ `preset=` และ `load=` จึงยืนยันจาก log ได้ว่าทุกบอร์ดรันชุดเดียวกัน
5. **image เดียวใช้ได้ทุกบอร์ด** role เป็น Automatic บอร์ดหาตัวเองจาก node list ตอนบูต ไม่ต้อง build แยก replica กับ client เหมือนเดิม
6. **ไม่มีสคริปต์ซ่อนค่า** ทุกอย่างอยู่ใน menuconfig พร้อม help text ทำซ้ำได้โดยไม่ต้องใช้เครื่องมือภายนอก

preset ไม่เปลี่ยนค่าที่ต้องเท่ากันทุก run และผู้ทดลองต้องตั้งเอง ได้แก่ role, ESP-NOW channel, TX power, timeout และขนาดบัฟเฟอร์

## 5. โหมดที่เพิ่มและดีกว่าเดิมอย่างไร

| โหมด | เดิม | ใหม่ | ทำไมดีกว่า |
|---|---|---|---|
| ESP-NOW interface | softAP (RF เปิดตลอด, มี beacon) | **Station** (ค่าเริ่มต้น) | ไม่มี beacon และเป็น interface เดียวที่ ESP-NOW หลับได้ |
| ESP-NOW receive power saving | ไม่มี | `_w50i100`, `_w110i200`, `_w50i200` | ฝั่ง ESP-NOW มีโหมดประหยัดพลังงานของตัวเอง (modem sleep) ทำให้เทียบ best vs best กับ UDP PS ได้ |
| ESP-NOW PHY rate | 1 Mbps อย่างเดียว | 1 และ **65 Mbps** (`en65*`) | ส่งเร็วขึ้น ใช้เวลาบนอากาศน้อยลง (65 Mbps ยังมีปัญหาลิงก์ของ node2 ดูหัวข้อ 7) |
| ESP-NOW retry | delay คงที่ ไม่ตรวจผล | รอ send callback, 3 attempts (`_t8` = 8) | ส่งเฟรมถัดไปเมื่อรู้ผลแล้ว ตามที่ ESP-IDF แนะนำ |
| UDP listen interval | ไม่ได้ตั้ง (3) | `udp_max3`, `udp_max10` | `PS_MAX` ต่างจาก `PS_MIN` จริง |
| UDP minimum active time | 50 ms (IDF) | `_a20` = 20 ms | ตื่นสั้นลงหลังมี traffic |
| UDP multicast | ปิด | `_mcast` (แยกเป็นจุดทดลองต่างหาก) | ทดสอบแบบส่งถึงทุกตัวในครั้งเดียว แต่ไม่ปนกับการเทียบแบบ unicast ที่มี ACK |
| TX power | 20 dBm | ตั้งได้ (`TBFT_WIFI_TX_POWER_QDBM`) | บอร์ดที่วางใกล้กันที่ 20 dBm ทำให้ ESP-NOW เสีย ACK 90–100 % แต่ที่ 8 dBm ไม่เสียเลย |
| ESP-NOW channel | 1 (ตายตัวในโค้ด) | ตั้งได้ ควรเป็นช่องที่ว่าง | บนช่องเดียวกับ AP ACK หาย 27 % และโหมด power saving ล้มทั้งกลุ่ม |

## 6. Scenario ใหม่ต่างจากเดิมเพราะอะไร

### 6.1 เดิม: interval 2, 10 และ 40 s หลัง reply

client รอ interval **หลัง** ได้ reply คาบจริงจึงเท่ากับ latency + interval ภาระงานของแต่ละ config จึงไม่เท่ากัน config ที่ช้ากว่าจะได้ request น้อยกว่า ในขณะที่บอร์ดยังกินไฟตอนว่างเท่าเดิม ค่า request ต่อจูลจึงลงโทษ config ที่ latency สูงซ้ำสองชั้น ยิ่ง interval สั้น (2 s) ภาระงานก็ยิ่งขึ้นกับ latency ของ config นั้นมากขึ้น

### 6.2 ใหม่: Experiment load สองแบบ

| Load | การส่ง | ตอบคำถาม |
|---|---|---|
| **Back-to-back** (`load=b2b`) | ส่ง request ถัดไปทันทีที่ request ก่อนหน้าเสร็จ มี request ค้างทีละหนึ่ง | consensus หนึ่งรอบใช้พลังงานเท่าไรเมื่อ cluster ทำงานเต็มที่ ไม่มีพลังงานตอนว่างปน |
| **ทุก 10 s** (`load=p10`) | ส่งตามเวลาสัมบูรณ์ request ที่ช้าไม่ทำให้ตารางเลื่อน ช่องที่ทับกันนับเป็น `missed` | ใช้งานจริงที่โหลดเบา config ไหนประหยัดที่สุด ตรงนี้ power saving มีผล |

เหตุผลที่ต้องมีทั้งสองแบบ:

- ที่โหลด 10 s บอร์ดว่างประมาณ 80 % ของเวลา และ CPU ทำงานที่ 160 MHz ตลอดเพราะไม่ใช้ light sleep ค่า request ต่อจูลจึงสะท้อนพลังงานตอนว่างเป็นหลัก
- ที่โหลด back-to-back ไม่มีเวลาว่าง config ที่ทำ consensus ได้เร็วกว่าจะได้ request ต่อจูลมากกว่าอย่างชัดเจน แต่โหมด power saving แทบไม่มีโอกาสหลับ
- config ที่ชนะในโหลดหนึ่งอาจไม่ชนะในอีกโหลด จึงต้องรายงานแยกกัน และห้ามรวม run ของสองโหลดในการจัดอันดับเดียว (ตั้งชื่อ run เป็น `<preset>` กับ `<preset>_p10`)

คาบคงที่แบบสัมบูรณ์แก้ปัญหาข้อ 6.1: จำนวนช่องในเวลาเท่ากันเท่ากันทุก config และ config ที่ช้าจะแสดงเป็น `missed` แยกจาก `fail` จึงไม่ถูกนับเป็นการสูญหาย

### 6.3 ตัวแปรที่กำหนดเท่ากันทุก config

TX power 8 dBm, view-change / Status / fetch / client re-send / reply timeout = 30 / 5 / 10 / 6 / 20 s, log ระดับ WARN, คิวรับ 16 ช่องทั้งสอง transport, CPU 160 MHz ไม่มี light sleep, บอร์ดชุดเดียวกัน และ role Automatic

## 7. ผลตรวจบนบอร์ด (2026-11-19, back-to-back, 8 บอร์ด, ประมาณ 75 s ต่อ preset)

ทุก preset ได้ fail = 0 และไม่มี view change สองตัวที่ replica ไม่ครบ:

| กลุ่ม | ผล |
|---|---|
| ESP-NOW 1 Mbps (5 preset) | 26–34 request, latency เฉลี่ย 1.72–2.23 s, replica ครบ 7 ตัว, ACK หาย 0–84 ครั้ง |
| ESP-NOW 65 Mbps (`en65`, `en65_w50i100`) | 37–39 request, 1.60–1.75 s, replica ครบ แต่ ACK หาย 1,041–3,893 ครั้ง |
| ESP-NOW 65 Mbps (`en65_w110i200`, `en65_w50i200`) | **node2 ไม่ได้ execute เลย** cluster ตอบได้ครบเพราะเหลือ 6 replica |
| UDP unicast (6 preset) | 20–39 request, 1.56–3.00 s, replica ครบ |
| UDP multicast (2 preset) | 31–33 request, 1.89–1.96 s, replica ครบ |

ผลข้างบนเป็นการตรวจว่าแต่ละ config ใช้งานได้ ยังไม่ใช่การวัดพลังงาน

## 8. ข้อจำกัดที่ยังเหลือ

1. **65 Mbps:** ลิงก์ระหว่าง node2 กับบอร์ดบางตัวส่งไม่ผ่าน ต้องย้ายบอร์ดหรือปรับ TX power (ให้เท่ากันทุก config) ก่อนใช้ผล `en65*`
2. **ESP-NOW broadcast ยังใช้ไม่ได้** ค้างที่ seqno 2
3. **ยังไม่มีการวัดพลังงาน** ต้องวัดทุกบอร์ด (เช่น INA219 ในสาย VBUS) รันอย่างน้อย 5 นาทีต่อ run, รัน config ที่ดีที่สุดซ้ำ ≥ 3 ครั้ง และบันทึก `energy.csv` ให้ `analyze.py`
4. **ปัจจัยที่ต่างกันโดยธรรมชาติ** ต้องระบุในรายงาน: ESP-NOW ใช้ช่องที่ว่าง ส่วน UDP ใช้ช่องของ AP, ไม่นับพลังงานของ router และค่าที่วัดเป็นพลังงานของทั้งบอร์ด (รวม regulator และ USB)
5. **"best" คือดีที่สุดในชุดที่ทดสอบ** ไม่ได้ค้นทุกค่า เช่น wake window ค่าอื่นนอก preset
