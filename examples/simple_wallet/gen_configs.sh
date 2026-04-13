#!/bin/bash
set -e

mkdir -p spiffs_image

echo "Generating RSA keys for 4 replicas and 1 client..."
for i in 0 1 2 3 4; do
    openssl genrsa -out spiffs_image/priv$i.pem 2048 2>/dev/null
    openssl rsa -in spiffs_image/priv$i.pem -pubout -out spiffs_image/pub$i.pem 2>/dev/null
    # TinyBFT expects DER format by default for mbedtls or we can use PEM. Let's provide PEM if it supports it, or DER if required.
    # The README says: Byz_init_replica("config.txt", "priv.der", ...). Wait, I will use DER to be safe.
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
# We will use dummy MACs, but in reality ESP-NOW nodes must know each other's MAC. 
# We'll put generic local admin MACs.
cat <<EOF > spiffs_image/config_espnow.txt
wallet
1
3000
5
239.0.0.1
node0 02:00:00:00:00:00 /spiffs/pub0.der
node1 02:00:00:00:00:01 /spiffs/pub1.der
node2 02:00:00:00:00:02 /spiffs/pub2.der
node3 02:00:00:00:00:03 /spiffs/pub3.der
client4 02:00:00:00:00:04 /spiffs/pub4.der
2000
5000
10000
EOF

echo "Done."
