#!/bin/bash
# Seed chroot curl/Python TLS defaults. Idempotent.
set -e
ROOTFS="${MACWS_ROOTFS:-/var/mnt/rootfs}"
CURLRC_SRC="/var/jb/usr/macOS/share/curl/curlrc"
CURLRC_DST="$ROOTFS/etc/curlrc"
BASHRC="$ROOTFS/Users/root/.bashrc"
MARKER="# MacWS: TLS defaults (LibreSSL/curl + PEM bundle)"

mkdir -p "$(dirname "$CURLRC_DST")" "$ROOTFS/Users/root"
if [ -f "$CURLRC_SRC" ]; then
  cp "$CURLRC_SRC" "$CURLRC_DST"
  chmod 644 "$CURLRC_DST"
fi

if [ ! -f "$BASHRC" ]; then
  printf '%s\n' '# MacWS: managed CLI environment' > "$BASHRC"
fi
if ! grep -qF "$MARKER" "$BASHRC" 2>/dev/null; then
  cat >> "$BASHRC" <<'SNIP'

# MacWS: TLS defaults (LibreSSL/curl + PEM bundle)
# SecureTransport SSLHandshake fails with errSSLBadCert in this chroot;
# NSURLSession works. Prefer curl's LibreSSL backend with the system PEM.
export SSL_CERT_FILE="${SSL_CERT_FILE:-/etc/ssl/cert.pem}"
export CURL_SSL_BACKEND="${CURL_SSL_BACKEND:-openssl}"
SNIP
fi
chmod 644 "$BASHRC" 2>/dev/null || true
echo "ensure_chroot_ssl_defaults: curlrc=$( [ -f "$CURLRC_DST" ] && echo ok || echo missing ) bashrc=ok"
