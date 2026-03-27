(function () {
  const UPLOAD_CHUNK_SIZE = 8 * 1024 * 1024;
  const DEFAULT_UPLOAD_STATUS = "请选择文件后上传";

  const state = {
    currentPath: "",
    cache: new Map(),
    controller: null,
    requestSeq: 0,
    sortKey: "modified_at",
    sortDirection: "desc"
  };

  const elements = {
    backButton: document.getElementById("back-button"),
    loading: document.getElementById("list-loading"),
    error: document.getElementById("list-error"),
    empty: document.getElementById("list-empty"),
    table: document.getElementById("file-table"),
    tableBody: document.getElementById("file-table-body"),
    sortButtons: Array.from(document.querySelectorAll(".sort-button")),
    uploadForm: document.getElementById("upload-form"),
    fileInput: document.getElementById("file-input"),
    uploadButton: document.querySelector("#upload-form button[type='submit']"),
    uploadProgress: document.getElementById("upload-progress"),
    uploadProgressBar: document.getElementById("upload-progress-bar"),
    uploadStatus: document.getElementById("upload-status")
  };

  function escapeHtml(value) {
    return String(value)
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll("\"", "&quot;")
      .replaceAll("'", "&#39;");
  }

  function buildPageUrl(path) {
    const params = new URLSearchParams();
    if (path) {
      params.set("path", path);
    }
    const query = params.toString();
    return query ? `/?${query}` : "/";
  }

  function buildApiUrl(path) {
    const params = new URLSearchParams();
    if (path) {
      params.set("path", path);
    }
    const query = params.toString();
    return query ? `/api/list?${query}` : "/api/list";
  }

  function buildChunkUploadUrl(path, fileName, uploadId, chunkIndex, totalChunks) {
    const params = new URLSearchParams();
    if (path) {
      params.set("path", path);
    }
    params.set("file", fileName);
    params.set("upload_id", uploadId);
    params.set("chunk_index", String(chunkIndex));
    params.set("total_chunks", String(totalChunks));
    return `/api/upload/chunk?${params.toString()}`;
  }

  function getPathFromLocation() {
    return new URLSearchParams(window.location.search).get("path") || "";
  }

  function readInitialDirectoryData() {
    const element = document.getElementById("initial-directory-data");
    if (!element) {
      return null;
    }

    const raw = element.textContent ? element.textContent.trim() : "";
    element.remove();
    if (!raw) {
      return null;
    }

    try {
      return JSON.parse(raw);
    } catch (error) {
      return null;
    }
  }

  function setPageLoading(loading) {
    document.body.classList.toggle("loading", loading);
    elements.loading.hidden = !loading;
  }

  function showError(message) {
    elements.loading.hidden = true;
    elements.error.textContent = message;
    elements.error.hidden = false;
    elements.empty.hidden = true;
    elements.table.hidden = true;
  }

  function clearError() {
    elements.error.hidden = true;
    elements.error.textContent = "";
  }

  function compareText(left, right) {
    return String(left || "").localeCompare(String(right || ""), "zh-CN", {
      numeric: true,
      sensitivity: "base"
    });
  }

  function compareItems(left, right) {
    const direction = state.sortDirection === "desc" ? -1 : 1;
    let result = 0;

    if (state.sortKey === "name") {
      result = compareText(left.name, right.name);
    } else if (state.sortKey === "type") {
      result = left.is_directory === right.is_directory ? 0 : left.is_directory ? -1 : 1;
    } else if (state.sortKey === "size") {
      result = Number(left.size_bytes || 0) - Number(right.size_bytes || 0);
    } else if (state.sortKey === "modified_at") {
      result = compareText(left.modified_at, right.modified_at);
    }

    if (result === 0 && state.sortKey !== "name") {
      result = compareText(left.name, right.name);
    }

    return result * direction;
  }

  function getSortedItems(items) {
    return [...items].sort(compareItems);
  }

  function updateSortIndicators() {
    elements.sortButtons.forEach((button) => {
      const key = button.dataset.sortKey || "";
      const indicator = button.querySelector(".sort-indicator");
      const isActive = key === state.sortKey;

      button.dataset.active = isActive ? "true" : "false";
      button.setAttribute("aria-pressed", isActive ? "true" : "false");

      if (!indicator) {
        return;
      }

      if (!isActive) {
        indicator.textContent = "";
        return;
      }

      indicator.textContent = state.sortDirection === "asc" ? "↑" : "↓";
    });
  }

  function setSort(key) {
    if (!key) {
      return;
    }

    if (state.sortKey === key) {
      state.sortDirection = state.sortDirection === "asc" ? "desc" : "asc";
    } else {
      state.sortKey = key;
      state.sortDirection = key === "modified_at" ? "desc" : "asc";
    }

    updateSortIndicators();

    if (state.cache.has(state.currentPath)) {
      renderList(state.cache.get(state.currentPath));
    }
  }

  function renderList(data) {
    state.currentPath = data.path || "";

    elements.backButton.hidden = state.currentPath === "";
    elements.backButton.dataset.path = data.parent_path || "";
    elements.loading.hidden = true;
    clearError();
    updateSortIndicators();

    if (!Array.isArray(data.items) || data.items.length === 0) {
      elements.empty.hidden = false;
      elements.table.hidden = true;
      elements.tableBody.innerHTML = "";
      return;
    }

    elements.empty.hidden = true;
    elements.table.hidden = false;
    elements.tableBody.innerHTML = getSortedItems(data.items).map((item) => {
      const name = escapeHtml(item.name);
      const sizeText = escapeHtml(item.size_text || "-");
      const modifiedAt = escapeHtml(item.modified_at || "");
      const badge = item.is_directory ? "目录" : "文件";

      if (item.is_directory) {
        const path = escapeHtml(item.path || "");
        return `
          <tr>
            <td><button class="name-button" type="button" data-path="${path}">${name}</button></td>
            <td><span class="badge">${badge}</span></td>
            <td>-</td>
            <td>${modifiedAt}</td>
            <td><button class="action-button table-action" type="button" data-path="${path}">进入</button></td>
          </tr>
        `;
      }

      const downloadUrl = escapeHtml(item.download_url || "#");
      return `
        <tr>
          <td>${name}</td>
          <td><span class="badge">${badge}</span></td>
          <td>${sizeText}</td>
          <td>${modifiedAt}</td>
          <td><a class="link-button table-action" href="${downloadUrl}">下载</a></td>
        </tr>
      `;
    }).join("");
  }

  async function loadDirectory(path, options) {
    const opts = options || {};
    const nextPath = path || "";
    const cacheKey = nextPath;
    const requestId = state.requestSeq + 1;
    state.requestSeq = requestId;

    if (state.controller) {
      state.controller.abort();
      state.controller = null;
    }

    if (!opts.force && state.cache.has(cacheKey)) {
      setPageLoading(false);
      renderList(state.cache.get(cacheKey));
      if (opts.pushState) {
        window.history.pushState({ path: nextPath }, "", buildPageUrl(nextPath));
      }
      return;
    }

    state.controller = new AbortController();
    setPageLoading(true);

    try {
      const response = await fetch(buildApiUrl(nextPath), {
        signal: state.controller.signal,
        headers: {
          "X-Requested-With": "fetch"
        }
      });
      const data = await response.json();

      if (requestId !== state.requestSeq) {
        return;
      }
      if (!response.ok || !data.ok) {
        throw new Error(data.message || "目录加载失败");
      }

      state.cache.set(cacheKey, data);
      renderList(data);
      if (opts.pushState) {
        window.history.pushState({ path: nextPath }, "", buildPageUrl(nextPath));
      }
    } catch (error) {
      if (error.name === "AbortError") {
        return;
      }
      showError(error.message || "目录加载失败");
    } finally {
      if (requestId === state.requestSeq) {
        state.controller = null;
        setPageLoading(false);
      }
    }
  }

  function resetUploadProgress() {
    elements.uploadProgress.hidden = true;
    elements.uploadProgressBar.style.width = "0%";
  }

  function formatFileSize(size) {
    const units = ["B", "KB", "MB", "GB", "TB"];
    let value = Number(size || 0);
    let unitIndex = 0;

    while (value >= 1024 && unitIndex + 1 < units.length) {
      value /= 1024;
      unitIndex += 1;
    }

    return `${value.toFixed(unitIndex === 0 ? 0 : 2)} ${units[unitIndex]}`;
  }

  function updateSelectedFileStatus() {
    if (!elements.fileInput.files || elements.fileInput.files.length === 0) {
      elements.uploadStatus.textContent = DEFAULT_UPLOAD_STATUS;
      return;
    }

    const file = elements.fileInput.files[0];
    const totalChunks = Math.max(1, Math.ceil(file.size / UPLOAD_CHUNK_SIZE));
    const sizeText = formatFileSize(file.size);
    elements.uploadStatus.textContent = totalChunks > 1
      ? `已选择 ${file.name} (${sizeText})`
      : `已选择 ${file.name} (${sizeText})`;
  }

  function setUploadBusy(busy) {
    elements.fileInput.disabled = busy;
    if (elements.uploadButton) {
      elements.uploadButton.disabled = busy;
    }
  }

  function createUploadId() {
    if (window.crypto && typeof window.crypto.randomUUID === "function") {
      return window.crypto.randomUUID().replaceAll("-", "");
    }
    return `${Date.now()}${Math.random().toString(16).slice(2)}`;
  }

  function parseJsonResponse(xhr) {
    try {
      return JSON.parse(xhr.responseText);
    } catch (error) {
      return null;
    }
  }

  function uploadChunk(file, start, end, url, onProgress) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", url, true);
      xhr.setRequestHeader("Content-Type", "application/octet-stream");

      xhr.upload.onprogress = function (progressEvent) {
        if (typeof onProgress === "function") {
          onProgress(progressEvent);
        }
      };

      xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4) {
          return;
        }

        const data = parseJsonResponse(xhr);
        if (xhr.status >= 200 && xhr.status < 300 && data && data.ok) {
          resolve(data);
          return;
        }

        reject(new Error((data && data.message) || "上传失败"));
      };

      xhr.onerror = function () {
        reject(new Error("上传失败，网络连接异常。"));
      };

      xhr.send(file.slice(start, end));
    });
  }

  async function uploadCurrentFile(event) {
    event.preventDefault();

    if (!elements.fileInput.files || elements.fileInput.files.length === 0) {
      elements.uploadStatus.textContent = DEFAULT_UPLOAD_STATUS;
      return;
    }

    const file = elements.fileInput.files[0];
    const totalBytes = file.size;
    const totalChunks = Math.max(1, Math.ceil(totalBytes / UPLOAD_CHUNK_SIZE));
    const uploadId = createUploadId();

    setUploadBusy(true);
    elements.uploadProgress.hidden = false;
    elements.uploadProgressBar.style.width = "0%";
    elements.uploadStatus.textContent = totalChunks > 1
      ? "开始上传，文件较大时会稍久一些..."
      : "开始上传...";

    let responseData = null;

    try {
      for (let chunkIndex = 0; chunkIndex < totalChunks; chunkIndex += 1) {
        const start = chunkIndex * UPLOAD_CHUNK_SIZE;
        const end = Math.min(totalBytes, start + UPLOAD_CHUNK_SIZE);
        const url = buildChunkUploadUrl(state.currentPath, file.name, uploadId, chunkIndex, totalChunks);

        responseData = await uploadChunk(file, start, end, url, function (progressEvent) {
          const uploadedBytes = start + progressEvent.loaded;
          const percentBase = totalBytes > 0 ? uploadedBytes / totalBytes : 1;
          const percent = Math.min(100, Math.round(percentBase * 100));
          elements.uploadProgressBar.style.width = `${percent}%`;
          elements.uploadStatus.textContent = totalChunks > 1
            ? `正在上传 ${percent}%，文件较大，请稍候。`
            : `正在上传 ${percent}%`;
        });
      }

      elements.uploadProgressBar.style.width = "100%";
      elements.uploadStatus.textContent = (responseData && responseData.message) || "上传成功";
      state.cache.delete(state.currentPath);
      elements.fileInput.value = "";
      window.setTimeout(function () {
        loadDirectory(state.currentPath, { force: true });
      }, 250);
    } catch (error) {
      resetUploadProgress();
      elements.uploadStatus.textContent = error && error.message ? error.message : "上传失败";
    } finally {
      setUploadBusy(false);
    }
  }

  elements.fileInput.addEventListener("change", updateSelectedFileStatus);
  document.addEventListener("click", function (event) {
    const target = event.target;
    if (!(target instanceof HTMLElement)) {
      return;
    }

    const sortButton = target.closest(".sort-button");
    if (sortButton instanceof HTMLButtonElement) {
      setSort(sortButton.dataset.sortKey || "");
      return;
    }

    const navTarget = target.closest("[data-path]");
    if (!navTarget) {
      return;
    }

    const path = navTarget.dataset.path || "";
    loadDirectory(path, { pushState: true });
  });

  window.addEventListener("popstate", function () {
    loadDirectory(getPathFromLocation(), { force: true });
  });

  elements.uploadForm.addEventListener("submit", uploadCurrentFile);

  const initialPath = getPathFromLocation();
  const initialData = readInitialDirectoryData();
  if (initialData && initialData.ok && (initialData.path || "") === initialPath) {
    state.cache.set(initialPath, initialData);
  }

  loadDirectory(initialPath, { force: !state.cache.has(initialPath) });
})();
