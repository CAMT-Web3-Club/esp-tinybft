#!/usr/bin/env bash
set -e

mkdir -p spiffs_image

echo "Generating RSA keys for 7 replicas and 1 client..."
for i in 0 1 2 3 4 5 6 7; do
    openssl genrsa -out spiffs_image/priv$i.pem 2048 2>/dev/null
    openssl rsa -in spiffs_image/priv$i.pem -pubout -out spiffs_image/pub$i.pem 2>/dev/null
    # -traditional forces PKCS#1 RSAPrivateKey DER.  OpenSSL 3.x defaults to
    # PKCS#8, and MbedTLS 4.x (ESP-IDF 6.x) cannot verify PKCS#8-parsed keys
    # with mbedtls_pk_check_pair() -> PSA_ERROR_INVALID_ARGUMENT (-135).
    if ! openssl rsa -in spiffs_image/priv$i.pem -traditional -outform DER -out spiffs_image/priv$i.der 2>/dev/null; then
        # OpenSSL < 3.0 has no -traditional flag, but its DER default is already PKCS#1
        openssl rsa -in spiffs_image/priv$i.pem -outform DER -out spiffs_image/priv$i.der 2>/dev/null
    fi
    openssl rsa -in spiffs_image/priv$i.pem -pubout -outform DER -out spiffs_image/pub$i.der 2>/dev/null
    rm spiffs_image/priv$i.pem spiffs_image/pub$i.pem
done

echo "Verifying key formats and pairs..."
for i in 0 1 2 3 4 5 6 7; do
    # privN.der must be PKCS#1 RSAPrivateKey: the second child of the outer
    # SEQUENCE is the modulus INTEGER; PKCS#8 would show a nested SEQUENCE
    # (rsaEncryption OID) instead.
    if ! openssl asn1parse -inform DER -in spiffs_image/priv$i.der 2>/dev/null | sed -n '3p' | grep -q "prim: INTEGER"; then
        echo "ERROR: spiffs_image/priv$i.der is not PKCS#1 RSAPrivateKey (PKCS#8 keys fail mbedtls_pk_check_pair on ESP-IDF 6.x)"
        exit 1
    fi
    # pubN.der and privN.der must belong to the same key pair
    priv_mod=$(openssl rsa -in spiffs_image/priv$i.der -inform DER -noout -modulus 2>/dev/null | sed 's/Modulus=//')
    pub_mod=$(openssl rsa -pubin -in spiffs_image/pub$i.der -inform DER -noout -modulus 2>/dev/null | sed 's/Modulus=//')
    if [ -z "$priv_mod" ] || [ "$priv_mod" != "$pub_mod" ]; then
        echo "ERROR: spiffs_image/pub$i.der does not match spiffs_image/priv$i.der"
        exit 1
    fi
done
echo "All keys verified: PKCS#1 private keys, matching pub/priv pairs."

echo "Generating UDP config..."
cat <<EOF > spiffs_image/config_udp.txt
counter
2
3000
8
239.0.0.1
node0 192.168.100.150 5000 /spiffs/pub0.der
node1 192.168.100.151 5000 /spiffs/pub1.der
node2 192.168.100.152 5000 /spiffs/pub2.der
node3 192.168.100.153 5000 /spiffs/pub3.der
node4 192.168.100.154 5000 /spiffs/pub4.der
node5 192.168.100.155 5000 /spiffs/pub5.der
node6 192.168.100.156 5000 /spiffs/pub6.der
client7 192.168.100.157 5000 /spiffs/pub7.der
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
