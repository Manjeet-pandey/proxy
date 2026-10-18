FROM gcc:13 AS build
WORKDIR /src
COPY . .
RUN apt-get update && apt-get install -y --no-install-recommends libboost-dev \
    && rm -rf /var/lib/apt/lists/*
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

FROM debian:bookworm-slim
COPY --from=build /src/build/edge-proxy /usr/local/bin/edge-proxy
EXPOSE 8080
ENTRYPOINT ["edge-proxy", "--listen", "8080", "--upstream", "127.0.0.1:9000"]
