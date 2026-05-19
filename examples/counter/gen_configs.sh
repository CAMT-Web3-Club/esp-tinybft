#!/usr/bin/env bash
set -e

mkdir -p spiffs_image

echo "Generating RSA keys for 7 replicas and 1 client..."
for i in 0 1 2 3 4 5 6 7; do
    openssl genrsa -out spiffs_image/priv$i.pem 2048 2>/dev/null
    openssl rsa -in spiffs_image/priv$i.pem -pubout -out spiffs_image/pub$i.pem 2>/dev/null
    openssl rsa -in spiffs_image/priv$i.pem -outform DER -out spiffs_image/priv$i.der 2>/dev/null
    openssl rsa -in spiffs_image/priv$i.pem -pubout -outform DER -out spiffs_image/pub$i.der 2>/dev/null
    rm spiffs_image/priv$i.pem spiffs_image/pub$i.pem
done

echo "Generating UDP config..."
cat <<EOF > spiffs_image/config_udp.txt
counter
2
3000
8
239.0.0.1
node0 127.0.0.1 5000 /spiffs/pub0.der
node1 127.0.0.1 5001 /spiffs/pub1.der
node2 127.0.0.1 5002 /spiffs/pub2.der
node3 127.0.0.1 5003 /spiffs/pub3.der
node4 127.0.0.1 5004 /spiffs/pub4.der
node5 127.0.0.1 5005 /spiffs/pub5.der
node6 127.0.0.1 5006 /spiffs/pub6.der
client7 127.0.0.1 5007 /spiffs/pub7.der
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
node0 02:00:00:00:00:00 /spiffs/pub0.der
node1 02:00:00:00:00:01 /spiffs/pub1.der
node2 02:00:00:00:00:02 /spiffs/pub2.der
node3 02:00:00:00:00:03 /spiffs/pub3.der
node4 02:00:00:00:00:04 /spiffs/pub4.der
node5 02:00:00:00:00:05 /spiffs/pub5.der
node6 02:00:00:00:00:06 /spiffs/pub6.der
client7 02:00:00:00:00:07 /spiffs/pub7.der
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
