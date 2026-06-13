#!/bin/bash
set -e

mkdir -p spiffs_image

echo "Generating RSA keys for 3 replicas and 1 client..."
for i in 0 1 2 3; do
    openssl genrsa -out spiffs_image/priv$i.pem 2048 2>/dev/null
    openssl rsa -in spiffs_image/priv$i.pem -pubout -out spiffs_image/pub$i.pem 2>/dev/null
    openssl pkcs8 -topk8 -nocrypt -in spiffs_image/priv$i.pem -outform DER -out spiffs_image/priv$i.der 2>/dev/null
    openssl pkey -in spiffs_image/priv$i.pem -pubout -outform DER -out spiffs_image/pub$i.der 2>/dev/null
    rm spiffs_image/priv$i.pem spiffs_image/pub$i.pem
done

echo "Generating UDP config..."
cat <<EOF > spiffs_image/config_udp.txt
wallet
1
3000
4
239.0.0.1
node0 127.0.0.1 5000 /spiffs/pub0.der
node1 127.0.0.1 5001 /spiffs/pub1.der
node2 127.0.0.1 5002 /spiffs/pub2.der
node3 127.0.0.1 5003 /spiffs/pub3.der
2000
5000
10000
EOF

echo "Generating ESP-NOW config (3 replicas + 1 client, f=1)..."
cat <<EOF > spiffs_image/config_espnow.txt
wallet
1
60000
4
0.0.0.0
node0 88:56:a6:5b:76:ac /spiffs/pub0.der
node1 e8:3d:c1:8c:18:58 /spiffs/pub1.der
node2 88:56:a6:5b:7c:ac /spiffs/pub2.der
client3 1c:db:d4:c6:41:14 /spiffs/pub3.der
60000
5000
10000
EOF

echo "Done."
echo ""
echo "Board → role mapping:"
echo "  node0 (primary) = ttyACM0  MAC 88:56:a6:5b:76:ac  → flash REPLICA firmware"
echo "  node1           = ttyACM1  MAC e8:3d:c1:8c:18:58  → flash REPLICA firmware"
echo "  node2           = ttyACM2  MAC 88:56:a6:5b:7c:ac  → flash REPLICA firmware"
echo "  client3         = ttyACM3  MAC 1c:db:d4:c6:41:14  → flash CLIENT firmware"
echo ""
echo "To build client firmware: idf.py -D CONFIG_EXAMPLE_ROLE_CLIENT=y build"