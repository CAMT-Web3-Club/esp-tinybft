#!/usr/bin/env bash
set -e

mkdir -p spiffs_image

echo "Generating ECDSA P-256 keys for 7 replicas and 1 client..."
for i in 0 1 2 3 4 5 6 7; do
    openssl ecparam -name prime256v1 -genkey -noout -out spiffs_image/priv$i.pem 2>/dev/null
    # Private: raw 32 bytes. ECPrivateKey DER is 30 77 02 01 01 04 20 [key] ...
    openssl ec -in spiffs_image/priv$i.pem -outform DER 2>/dev/null | tail -c +8 | head -c 32 > spiffs_image/priv$i.der
    # Public: raw 65 bytes uncompressed (0x04||X||Y). SPKI DER last 65 bytes.
    openssl ec -in spiffs_image/priv$i.pem -pubout -outform DER 2>/dev/null | tail -c 65 > spiffs_image/pub$i.der
    rm spiffs_image/priv$i.pem
done

echo "Generating UDP config..."
cat <<EOF > spiffs_image/config_udp.txt
counter
2
3000
8
239.0.0.1
node0 192.168.100.109 5000 /spiffs/pub0.der
node1 192.168.100.107 5000 /spiffs/pub1.der
node2 192.168.100.106 5000 /spiffs/pub2.der
node3 192.168.100.104 5000 /spiffs/pub3.der
node4 192.168.100.108 5000 /spiffs/pub4.der
node5 192.168.100.105 5000 /spiffs/pub5.der
node6 192.168.100.194 5000 /spiffs/pub6.der
client7 192.168.100.190 5000 /spiffs/pub7.der
2000
5000
10000
EOF

echo "Generating ESP-NOW config (7 replicas + 1 client, f=2)..."
cat <<EOF > spiffs_image/config_espnow.txt
counter
2
60000
8
0.0.0.0
node0 88:56:a6:5b:f2:f0 /spiffs/pub0.der
node1 88:56:a6:5b:76:84 /spiffs/pub1.der
node2 1c:db:d4:c6:40:98 /spiffs/pub2.der
node3 1c:db:d4:c5:7f:20 /spiffs/pub3.der
node4 1c:db:d4:c6:40:40 /spiffs/pub4.der
node5 88:56:a6:5b:6b:2c /spiffs/pub5.der
node6 88:56:a6:5b:78:dc /spiffs/pub6.der
client7 e8:3d:c1:8c:18:58 /spiffs/pub7.der
60000
5000
10000
EOF



echo "Done."
echo ""
echo "Board → role mapping:"
echo "  node0..node6 = replicas (7 boards) → flash REPLICA firmware"
echo "  client7      = client  (1 board)  → flash CLIENT firmware"
echo ""
echo "Edit spiffs_image/config_espnow.txt with real MACs before building."
echo "To build client firmware: idf.py -D CONFIG_EXAMPLE_ROLE_CLIENT=y build"
