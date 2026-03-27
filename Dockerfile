FROM docker.1ms.run/ubuntu:24.04

ENV TZ=Asia/Shanghai
RUN ln -sf /usr/share/zoneinfo/$TZ /etc/localtime \
    && echo "$TZ" > /etc/timezone

RUN mkdir -p /app/frontend
RUN mkdir -p /app/dir
RUN mkdir -p /app/logs
WORKDIR /app

COPY build/file_transfer_hub .
COPY frontend ./frontend

CMD ["/app/file_transfer_hub", "/app/dir", "8080", "/app/logs"]
