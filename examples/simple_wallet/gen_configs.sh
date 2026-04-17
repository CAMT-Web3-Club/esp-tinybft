#!/bin/bash
set -e

mkdir -p spiffs_image

echo "Generating RSA keys for 4 replicas and 1 client..."
for i in 0 1 2 3 4; do
    openssl genrsa -out spiffs_image/priv$i.pem 2048 2>/dev/null
    openssl rsa -in spiffs_image/priv$i.pem -pubout -out spiffs_image/pub$i.pem 2>/dev/null
    openssl rsa -in spiffs_image/priv$i.pem -outform DER -out spiffs_image/priv$i.der 2>/dev/null
    openssl rsa -in spiffs_image/priv$i.pem -pubout -outform DER -out spiffs_image/pub$i.der 2>/dev/null
    rm spiffs_image/priv$i.pem spiffs_image/pub$i.pem
done

echo "Generating UDP config..."
cat <<EOF > spiffs_image/config_udp.txt
wallet
1
3000
5
239.0.0.1
node0 127.0.0.1 5000 /spiffs/pub0.der
node1 127.0.0.1 5001 /spiffs/pub1.der
node2 127.0.0.1 5002 /spiffs/pub2.der
node3 127.0.0.1 5003 /spiffs/pub3.der
client4 127.0.0.1 5004 /spiffs/pub4.der
2000
5000
10000
EOF

echo "Generating ESP-NOW config..."
echo "NOTE: Replace MAC addresses below with your boards' real MACs."
echo "      Run 'esptool -p /dev/ttyACMx flash_id' on each board to find its MAC."
echo "      The config has 4 nodes (all replicas, no client entry)."
cat <<EOF > spiffs_image/config_espnow.txt
wallet
1
3000
4
0.0.0.0
node0 88:56:a6:5b:f2:f0 /spiffs/pub0.der
node1 1c:db:d4:c6:41:14 /spiffs/pub1.der
node2 1c:db:d4:c5:7f:20 /spiffs/pub2.der
node3 88:56:a6:5b:76:84 /spiffs/pub3.der
2000
5000
10000
EOF

echo "Done."
echo ""
echo "!!! IMPORTANT: Edit spiffs_image/config_espnow.txt to replace MAC addresses"
echo "    with your boards' real WiFi STA MACs before building. !!!"