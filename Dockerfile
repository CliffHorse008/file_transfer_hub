FROM docker.1ms.run/ubuntu:24.04

ENV TZ=Asia/Shanghai
RUN apt-get update && apt-get install -y --no-install-recommends tzdata \
    && ln -snf /usr/share/zoneinfo/$TZ /etc/localtime \
    && echo $TZ > /etc/timezone \
    && rm -rf /var/lib/apt/lists/*  # 清理缓存，减小镜像体积

RUN mkdir -p /app/frontend
RUN mkdir -p /app/dir
RUN mkdir -p /app/logs
WORKDIR /app

COPY build/file_transfer_hub .
COPY frontend ./frontend

CMD ["/app/file_transfer_hub", "/app/dir", "8080", "/app/logs"]
