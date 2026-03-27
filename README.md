# 文件中转站

这是一个使用 C 语言实现的轻量文件中转站，使用 CMake 构建，当前为前后端分离结构：

- `frontend/` 中是独立前端页面与脚本
- `src/main.c` 中是 C 语言后端服务
- 后端提供目录列表、上传、下载 API

## 构建

```bash
cmake -S . -B build
cmake --build build
```

## 运行

```bash
./build/file_transfer_hub [文件目录] [端口] [日志目录]
```

例如：

```bash
./build/file_transfer_hub ./ 8080 ./logs
```

然后在浏览器中访问：

```text
http://127.0.0.1:8080
```

## Docker 构建

```bash
docker build -t file-transfer-hub .
```

## 说明

- 默认监听端口是 `8080`
- 默认展示当前工作目录
- 默认日志写入当前工作目录，可通过第三个启动参数单独指定日志目录
- 指定的日志目录不存在时会自动创建
- 前端默认使用分片上传，支持大文件传输
- 兼容 `multipart/form-data` 单次上传
- 上传和下载都限制在启动时指定的目录根路径内
- 前端静态资源默认从项目下的 `frontend/` 目录加载

## API

- `GET /api/list?path=<相对目录>`: 获取目录内容
- `POST /api/upload?path=<相对目录>`: 上传文件
- `POST /api/upload/chunk?path=<相对目录>&file=<文件名>&upload_id=<会话ID>&chunk_index=<分片序号>&total_chunks=<分片总数>`: 分片上传
- `GET /api/download?path=<相对目录>&file=<文件名>`: 下载文件
