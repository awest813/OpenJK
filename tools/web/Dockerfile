# Web build of OpenJK plus the server that hosts it and relays its network
# traffic (tools/web/server.js). From the repository root:
#
#   docker build -f tools/web/Dockerfile -t openjk-web .
#   docker run -p 8080:8080 openjk-web
#
# then open http://localhost:8080/. Put a TLS-terminating proxy in front of
# it for anything but local use. Options after the image name are passed to
# server.js, e.g. `docker run -p 8080:8080 openjk-web --allow jka.example.com:29070`.

FROM emscripten/emsdk:6.0.11 AS build

COPY . /src
RUN emcmake cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release && \
	cmake --build /build -j "$(nproc)"

FROM node:22-alpine

COPY --from=build /build/index.html /build/openjk_sp.wasm32.* /build/openjk.wasm32.* /srv/openjk/
COPY tools/web/server.js /opt/openjk/server.js

USER node
EXPOSE 8080
ENTRYPOINT ["node", "/opt/openjk/server.js", "--root", "/srv/openjk", "--port", "8080"]
