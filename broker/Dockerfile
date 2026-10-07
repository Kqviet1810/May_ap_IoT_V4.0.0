# MAYAP MQTT broker. Build context = repository root:
#   docker build -f broker/Dockerfile -t mayap-mqtt-broker .
FROM node:22-alpine
WORKDIR /app
COPY broker/package.json broker/package-lock.json broker/
RUN npm ci --prefix broker --omit=dev --ignore-scripts --no-audit --no-fund
# The broker core is shared with the Cloudflare-free test harness: same files, no copy.
COPY cloudflare/package.json cloudflare/package.json
COPY cloudflare/src/broker cloudflare/src/broker
COPY broker/server.js broker/runtime-node.js broker/
RUN mkdir -p /data && chown node:node /data
USER node
ENV BIND_HOST=0.0.0.0 MQTT_TLS_PORT=8883 WS_PORT=8443 BROKER_DATA_FILE=/data/state.json \
    TLS_CERT_FILE=/certs/fullchain.pem TLS_KEY_FILE=/certs/privkey.pem
VOLUME ["/data"]
EXPOSE 8883 8443
CMD ["node", "broker/server.js"]
