FROM docker.1ms.run/ubuntu:24.04

RUN mkdir -p /app/frontend
RUN mkdir -p /app/dir
WORKDIR /app

COPY build/file_transfer_hub .
COPY frontend ./frontend

CMD ["/app/file_transfer_hub", "/app/dir", "8080"]
