    let apiQueue = Promise.resolve();

    async function getJson(url, options) {
      const result = apiQueue.then(() => getJsonRaw(url, options));
      apiQueue = result.then(() => undefined, () => undefined);
      return result;
    }

    async function getJsonRaw(url, options) {
      const method = String(options?.method || "GET").toUpperCase();
      const maxRetries = method === "GET" ? FETCH_RETRY_MAX : 0;

      for (let attempt = 0; attempt <= maxRetries; attempt++) {
        const controller = typeof AbortController === "function" ? new AbortController() : null;
        const timeoutMs = Number(options?.timeoutMs) > 0 ? Number(options.timeoutMs) : FETCH_TIMEOUT_MS;
        const timeoutId = controller ? setTimeout(() => controller.abort(), timeoutMs) : null;
        try {
          const requestOptions = controller ? { ...(options || {}), signal: controller.signal } : options;
          const response = await fetch(url, requestOptions);
          const text = await response.text();
          const { body, parseError } = tryParseJson(text);

          if (!response.ok) {
            if (response.status === 401) {
              /* 会话失效(Cookie 过期/换浏览器/设备重启)→ 回页面内登录表单。
               * 服务端已按产品决策不再下发 WWW-Authenticate,故不会再弹系统认证框。
               * 见 问题与版本优化.md 1.3-22 */
              window.location.href = "/portal";
              return;
            }
            const err = new Error(body?.message || body?.error || text || `HTTP ${response.status}`);
            err.body = body;
            err.status = response.status;
            throw err;
          }

          if (!text || body === null) {
            const err = new Error("empty_response");
            err.status = response.status;
            throw err;
          }

          if (parseError) {
            const err = new Error(body?.message || "invalid_response");
            err.status = response.status;
            err.body = body;
            throw err;
          }

          notePollOk();
          return body;
        } catch (error) {
          /* 1.3-36:设备 httpd 在堆紧张(MQTT 重连风暴等)时会回 503,
           * 且此前 503 因带 status 从不重试 → 面板永久缺失。503/502/504、
           * 空响应/截断响应与连接级瞬断一样按可重试处理。 */
          const retryableStatus = error?.status === 502 ||
                                  error?.status === 503 ||
                                  error?.status === 504;
          const retryableBody = error?.message === "empty_response" ||
                                error?.message === "invalid_response";
          const canRetry = attempt < maxRetries &&
                           (retryableStatus || retryableBody ||
                            (!error?.status && isTransientFetchError(error)));
          if (!canRetry) {
            throw error;
          }
          await sleep(FETCH_RETRY_DELAY_MS * (attempt + 1));
        } finally {
          if (timeoutId) {
            clearTimeout(timeoutId);
          }
        }
      }
    }

    async function loadDevice() {
      const data = await getJson("/api/v1/device/status");
      renderDevice(data);
    }

    function renderDevice(data) {
      const appPartition = data?.app_partition || {};
      const storage = data?.storage || {};
      const network = data?.network || {};
      const bluetooth = data?.bluetooth || {};
      const mqtt = data?.mqtt || {};

      renderKeyValues("deviceInfo", {
        project: data?.project || "-",
        app_version: data?.app_version || "-",
        target: data?.idf_target || "-",
        idf_version: data?.idf_version || "-",
        mac: data?.mac || "-",
        net_mode: humanMode(network.mode),
        ap_ip: network.ap_ip || "-",
        sta_ip: network.sta_ip || "-",
        bt_mode: humanBtMode(bluetooth.runtime_mode),
        mqtt: humanMqttState(mqtt.connected, mqtt.last_error),
        free_heap: formatBytes(data?.free_heap),
        app_partition: formatPartitionLabel(appPartition.label, appPartition.total_bytes),
        app_usage: formatUsage(appPartition.used_bytes, appPartition.total_bytes),
        storage_usage: formatUsage(storage.used_bytes, storage.total_bytes)
      });
    }

    async function loadNetwork() {
      const data = await getJson("/api/v1/config/network");
      const network = data || {};
      state.networkConfig = network;
      document.getElementById("netMode").value = network.mode || "ap";
      document.getElementById("ssid").value = network.ap_ssid || "";
      /* 凭据不回传明文:输入框留空 = 保持已存值 */
      document.getElementById("password").value = "";
      document.getElementById("apPasswordClear").checked = false;
      document.getElementById("staSsid").value = network.sta_ssid || "";
      document.getElementById("staPassword").value = "";
      document.getElementById("staPasswordClear").checked = false;
      renderSavedWifiProfiles(network.saved_sta_profiles || []);
      renderScanResults([]);
      renderNetworkStatus(network);
      updateNetworkValidationStatus();
    }

    function networkModeHasSta(mode) {
      return mode === "sta" || mode === "apsta";
    }

    /* 静态 IP 默认值:优先取当前 DHCP 实际拿到的地址(把"当前地址固定下来"
     * 是静态 IP 最常见的用法),未联网时退回示例值。只填空白字段,不覆盖已填内容。
     * 1.3-52:主表单的静态控件已移除,本函数仅供连接向导的静态面板使用。 */
    function staticIpDefaults() {
      const net = state.networkConfig || {};
      const hasIp = net.sta_ip && net.sta_ip !== "0.0.0.0" && isValidIpv4(net.sta_ip);
      const hasGw = net.sta_gateway && net.sta_gateway !== "0.0.0.0" && isValidIpv4(net.sta_gateway);
      const hasMask = net.sta_netmask && net.sta_netmask !== "0.0.0.0" && isValidIpv4(net.sta_netmask);
      const ip = hasIp ? net.sta_ip : "192.168.1.213";
      const gw = hasGw ? net.sta_gateway : "192.168.1.1";
      const mask = hasMask ? net.sta_netmask : "255.255.255.0";
      return { ip, gw, mask, dns1: gw, dns2: "8.8.8.8" };
    }

    function isValidIpv4(value) {
      if (!value || typeof value !== "string") {
        return false;
      }
      const parts = value.split(".");
      if (parts.length !== 4) {
        return false;
      }
      return parts.every((part) => {
        if (!/^(0|[1-9]\d*)$/.test(part)) {
          return false;
        }
        const num = Number(part);
        return num >= 0 && num <= 255;
      });
    }

    function renderNetworkStatus(data) {
      const network = data || {};
      let summary = t("network.summary", {
        mode: humanMode(network.mode),
        ap: `${network.ap_ssid || t("network.none")}@${network.ap_ip || t("network.none")}`,
        sta: network.sta_ssid || t("network.none"),
        staIp: network.sta_has_ip ? network.sta_ip : t("network.noIp"),
        lastDisconnect: network.last_disconnect || t("network.none")
      });
      /* 已存 STA 名称却仍是纯 AP = 上游永远连不上(1.3-04 实测):摘要后追加显式告警 */
      if (network.sta_ssid && !networkModeHasSta(network.mode)) {
        summary += `
⚠ ${t("network.staSsidInactive")}`;
      }
      /* 静态 IP 冲突:摘要加告警 + 弹窗提示去修改(1.3-35) */
      if (network.sta_ip_conflict) {
        summary += `
${t("network.ipConflictSummary", { ip: network.sta_ip || "-" })}`;
        showIpConflictModal(network.sta_ip || "");
      }
      setStatus("networkStatus", summary, network.sta_has_ip && !network.sta_ip_conflict ? "ok" : "warn");
    }

    function clearNetworkInputErrors() {
      ["ssid", "password", "staSsid", "staPassword"].forEach((id) => {
        const el = document.getElementById(id);
        if (el) {
          el.classList.remove("input-error");
        }
      });
    }

    function markNetworkInputError(id) {
      const el = document.getElementById(id);
      if (el) {
        el.classList.add("input-error");
      }
    }

    /* 前端当场校验:与后端统一层规则对齐(长度、必填、开放网络显式清除、静态 IP 格式)。
     * 返回 {ok, message};ok=true 时不改状态。 */
    function validateNetworkForm() {
      const mode = document.getElementById("netMode").value;
      const apSsid = document.getElementById("ssid").value.trim();
      const apPassword = document.getElementById("password").value;
      const apClear = document.getElementById("apPasswordClear").checked;
      const staSsid = document.getElementById("staSsid").value.trim();
      const staPassword = document.getElementById("staPassword").value;
      const staClear = document.getElementById("staPasswordClear").checked;
      const current = state.networkConfig || {};

      clearNetworkInputErrors();

      if ((mode === "ap" || mode === "apsta") && !apSsid) {
        markNetworkInputError("ssid");
        return { ok: false, message: t("network.apSsidRequired") };
      }
      if (apPassword && apPassword.length < 8) {
        markNetworkInputError("password");
        return { ok: false, message: t("network.passwordTooShort") };
      }
      if ((mode === "ap" || mode === "apsta") && apSsid &&
          apSsid !== (current.ap_ssid || "") &&
          !current.password_enabled && !apClear && !apPassword) {
        markNetworkInputError("password");
        return { ok: false, message: t("network.passwordRequired") };
      }

      if ((mode === "sta" || mode === "apsta") && !staSsid) {
        markNetworkInputError("staSsid");
        return { ok: false, message: t("network.staSsidRequired") };
      }
      if (staSsid && !staClear) {
        if (staPassword) {
          if (staPassword.length < 8) {
            markNetworkInputError("staPassword");
            return { ok: false, message: t("network.passwordTooShort") };
          }
        } else if (!current.sta_password_set) {
          markNetworkInputError("staPassword");
          return { ok: false, message: t("network.passwordRequired") };
        }
      }

      /* 静态 IP 的填写与校验已随 1.3-51/52 移入连接向导,这里不再检查 */
      return { ok: true, message: "" };
    }

    function updateNetworkValidationStatus() {
      const result = validateNetworkForm();
      const saveButton = document.getElementById("saveNetwork");
      if (!result.ok) {
        setNetworkValidationStatus(result.message, "bad");
        saveButton.disabled = true;
      } else {
        setNetworkValidationStatus(t("network.validationOk"), "ok");
        saveButton.disabled = false;
      }
      return result;
    }
