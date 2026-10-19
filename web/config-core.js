    function detectLanguage() {
      const saved = localStorage.getItem("iothub_lang");
      if (saved === "zh-CN" || saved === "en") {
        return saved;
      }
      return navigator.language && navigator.language.toLowerCase().startsWith("zh") ? "zh-CN" : "en";
    }

    state.language = detectLanguage();

    function t(key, vars = {}) {
      const dict = translations[state.language] || translations["zh-CN"];
      const fallback = translations["zh-CN"];
      const template = dict[key] ?? fallback[key] ?? key;
      return template.replace(/\{(\w+)\}/g, (_, name) => vars[name] ?? "");
    }

    function humanMode(mode) {
      if (!mode) {
        return "-";
      }
      const key = `mode.${mode}`;
      const text = t(key);
      return text === key ? mode : text;
    }

    function humanBtMode(mode) {
      if (!mode) {
        return "-";
      }
      const key = `bt.${mode}`;
      const text = t(key);
      return text === key ? mode : text;
    }

    function humanMqttState(connected, lastError) {
      if (connected) {
        return t("state.connected");
      }
      if (!lastError) {
        return t("state.disconnected");
      }

      const key = `state.${lastError}`;
      const text = t(key);
      return text === key ? lastError : text;
    }

    function applyTranslations() {
      document.documentElement.lang = state.language;
      document.title = t("page.title");
      document.getElementById("languageSelect").value = state.language;
      document.querySelectorAll("[data-i18n]").forEach((element) => {
        element.textContent = t(element.dataset.i18n);
      });
      document.querySelectorAll("[data-i18n-placeholder]").forEach((element) => {
        element.placeholder = t(element.dataset.i18nPlaceholder);
      });
    }

    function setStatus(id, text, tone) {
      const element = document.getElementById(id);
      element.textContent = text;
      element.className = "status" + (tone ? ` ${tone}` : "");
    }

    function setNetworkValidationStatus(text, tone) {
      const element = document.getElementById("networkValidation");
      if (!text) {
        element.hidden = true;
        element.textContent = "";
        element.className = "status";
        return;
      }
      element.hidden = false;
      element.textContent = text;
      element.className = "status" + (tone ? ` ${tone}` : "");
    }

    function renderUartConsole() {
      const rxFormat = document.getElementById("uartRxFormat");
      const txFormat = document.getElementById("uartTxFormat");
      const consoleBox = document.getElementById("uartRxConsole");
      let renderedValue = "";
      if (!consoleBox) {
        return;
      }

      if (rxFormat) {
        rxFormat.value = state.uartRxFormat;
      }
      if (txFormat) {
        txFormat.value = state.uartTxFormat;
      }

      renderedValue = state.uartRxFormat === "hex"
        ? (state.uartConsoleHex || "")
        : (state.uartConsolePlain || "");
      consoleBox.value = renderedValue;
      consoleBox.scrollTop = consoleBox.scrollHeight;
    }

    function renderKeyValues(targetId, data) {
      const target = document.getElementById(targetId);
      target.innerHTML = Object.entries(data).map(([key, value]) => {
        return `<div class="row"><span>${t(`device.${key}`)}</span><strong>${value}</strong></div>`;
      }).join("");
    }

    function getErrorMessage(error) {
      const statusPrefix = error?.status ? `HTTP ${error.status}: ` : "";
      if (error?.message === "empty_response") {
        return `${statusPrefix}${t("error.emptyResponse")}`;
      }
      if (error?.message === "invalid_response") {
        return `${statusPrefix}${t("error.invalidResponse")}`;
      }
      if (isTransientFetchError(error)) {
        return t("error.transientFetch");
      }
      const message = error?.message || String(error) || t("network.none");
      return `${statusPrefix}${message}`;
    }

    function sleep(ms) {
      return new Promise((resolve) => setTimeout(resolve, ms));
    }

    function isTransientFetchError(error) {
      if (error && (error.name === "AbortError" || error.timeout === true)) {
        return true;   /* 超时(AbortController)按可重试处理 */
      }
      const message = String(error?.message || error || "");
      return message === "Failed to fetch" ||
             message === "Load failed" ||
             message === "NetworkError when attempting to fetch resource." ||
             message === "The operation was aborted.";
    }

    function renderDeviceError(message) {
      renderKeyValues("deviceInfo", {
        error: message
      });
    }

    async function runWithPanelError(task, onError) {
      try {
        await task();
      } catch (error) {
        onError(getErrorMessage(error));
      }
    }

    function formatBytes(bytes) {
      const value = Number(bytes) || 0;
      if (value >= 1024 * 1024) {
        return `${(value / (1024 * 1024)).toFixed(2)} MiB`;
      }
      if (value >= 1024) {
        return `${(value / 1024).toFixed(1)} KiB`;
      }
      return `${value} B`;
    }

    function formatUsage(usedBytes, totalBytes) {
      const used = Number(usedBytes);
      const total = Number(totalBytes);
      if (!Number.isFinite(used) || !Number.isFinite(total) || total <= 0) {
        return "-";
      }
      const free = total > used ? total - used : 0;
      const percent = ((used * 100) / total).toFixed(1);
      return `${formatBytes(used)} / ${formatBytes(total)} (${percent}%), ${t("usage.free")} ${formatBytes(free)}`;
    }

    function formatPartitionLabel(label, totalBytes) {
      if (!label) {
        return "-";
      }
      const total = Number(totalBytes);
      return Number.isFinite(total) && total > 0
        ? `${label} (${formatBytes(total)})`
        : label;
    }

    function formatGpioLabel(gpio) {
      return Number.isFinite(Number(gpio)) ? `GPIO${gpio}` : "GPIO-";
    }

    function escapeHtml(value) {
      return String(value ?? "")
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#39;");
    }

    function renderUartBaudList() {
      const select = document.getElementById("uartBaudSelect");
      if (!select) {
        return;
      }

      select.innerHTML = state.uartCommonBaudrates.map((baudrate) =>
        `<option value="${baudrate}">${baudrate}</option>`).join("");
    }

    function openWifiScanModal() {
      document.getElementById("wifiScanModal").hidden = false;
    }

    function closeWifiScanModal() {
      document.getElementById("wifiScanModal").hidden = true;
    }

    function openSavedWifiModal() {
      document.getElementById("savedWifiModal").hidden = false;
    }

    function closeSavedWifiModal() {
      document.getElementById("savedWifiModal").hidden = true;
    }

    /* 静态 IP 冲突弹窗:状态轮询/加载时发现 sta_ip_conflict=true 即弹出(一次) */
    function showIpConflictModal(ip) {
      if (state.ipConflictShown) {
        return;
      }
      state.ipConflictShown = true;
      document.getElementById("ipConflictText").textContent =
        t("network.ipConflictText", { ip: ip || "-" });
      document.getElementById("ipConflictModal").hidden = false;
    }

    function closeIpConflictModal() {
      document.getElementById("ipConflictModal").hidden = true;
    }

    function applySavedWifiProfile(index) {
      const profile = state.savedWifiProfiles[index];
      if (!profile) {
        return;
      }
      const current = state.networkConfig || {};
      if (current.sta_connected && current.sta_ssid === profile.ssid) {
        setStatus("networkStatus", t("network.profileAlreadyConnected", {
          ssid: profile.ssid || t("scan.hidden")
        }), "ok");
        return;
      }
      document.getElementById("staSsid").value = profile.ssid || "";
      document.getElementById("staPassword").value = "";
      closeSavedWifiModal();
      setStatus("networkStatus", t("network.profileApplied", {
        ssid: profile.ssid || t("scan.hidden")
      }), "ok");
    }

    function applyScannedWifi(index) {
      const profile = state.wifiScanResults[index];
      if (!profile) {
        return;
      }
      openWifiJoinModal(profile.ssid || "");
    }

    /* -------------------------------------------------------------------------
     * 连接 Wi-Fi 向导(1.3-51):扫描列表点“使用”后进入。
     * SSID 自动填入(可手动改),密码需“验证密码”(真实试连)通过后才出现“保存”;
     * DHCP / 静态 IP 二选一(默认 DHCP),勾选静态 IP 展开静态地址面板。
     * “保存”把向导字段回填到主网络表单并走既有 saveNetwork() 全流程
     * (含静态 IP 校验/模式推断/保存后重启确认),不复制保存逻辑。
     * ------------------------------------------------------------------------- */
    function openWifiJoinModal(ssid) {
      document.getElementById("joinSsid").value = ssid || "";
      document.getElementById("joinPassword").value = "";
      document.getElementById("joinIpDhcp").checked = true;
      document.getElementById("joinStaticPanel").hidden = true;
      document.getElementById("joinSave").hidden = true;
      state.joinVerified = null;   /* 1.3-52:换一个 Wi-Fi 就必须重新验证密码 */
      setJoinValidationStatus("", "");
      document.getElementById("wifiJoinModal").hidden = false;
    }

    function closeWifiJoinModal() {
      document.getElementById("wifiJoinModal").hidden = true;
    }

    function joinIpMode() {
      return document.getElementById("joinIpStatic").checked ? "static" : "dhcp";
    }

    function setJoinValidationStatus(text, tone) {
      const element = document.getElementById("joinValidation");
      if (!text) {
        element.hidden = true;
        element.textContent = "";
        element.className = "status";
        return;
      }
      element.hidden = false;
      element.textContent = text;
      element.className = "status" + (tone ? ` ${tone}` : "");
    }

    function updateJoinStaticPanel() {
      const panel = document.getElementById("joinStaticPanel");
      panel.hidden = joinIpMode() !== "static";
      if (panel.hidden) {
        return;
      }
      /* 与主表单同一套默认值策略:留空字段按当前 DHCP 地址/示例值填充 */
      const defaults = staticIpDefaults();
      const fill = (id, value) => {
        const el = document.getElementById(id);
        if (el && !el.value.trim()) {
          el.value = value;
        }
      };
      fill("joinStaticIp", defaults.ip);
      fill("joinStaticGateway", defaults.gw);
      fill("joinStaticNetmask", defaults.mask);
      fill("joinStaticDns1", defaults.dns1);
      fill("joinStaticDns2", defaults.dns2);
    }

    async function verifyJoinWifi() {
      const ssid = document.getElementById("joinSsid").value.trim();
      const password = document.getElementById("joinPassword").value;
      if (!ssid) {
        setJoinValidationStatus(t("network.staSsidRequired"), "bad");
        return;
      }
      if (password && password.length < 8) {
        setJoinValidationStatus(t("network.passwordTooShort"), "bad");
        return;
      }
      document.getElementById("joinSave").hidden = true;
      stopPolling();
      setJoinValidationStatus(t("wifiJoin.verifying"), "warn");
      try {
        await getJson("/api/v1/config/network/tryconnect", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ sta_ssid: ssid, sta_password: password }),
          timeoutMs: 25000
        });
        setJoinValidationStatus(t("network.tryConnectOk"), "ok");
        document.getElementById("joinSave").hidden = false;
        state.joinVerified = {
          ssid: ssid,
          password: password
        };
      } catch (error) {
        const reason = error?.message || "connect_failed";
        const text = t("network.try." + reason);
        setJoinValidationStatus(
          text === "network.try." + reason
            ? t("network.tryConnectFailed", { reason })
            : t("network.tryConnectFailed", { reason: text }), "bad");
      } finally {
        startStatusPolling();
        startUartConsolePolling();
      }
    }

    async function saveJoinWifi() {
      /* 1.3-52:向导直接组 payload(不再回填主表单)。
       * AP 部分沿用主表单当前值;STA 与静态 IP 按向导选择;SSID/密码若在
       * "验证密码"通过后又被修改,必须重新验证。 */
      const joinSsid = document.getElementById("joinSsid").value.trim();
      const joinPassword = document.getElementById("joinPassword").value;
      const verified = state.joinVerified;
      if (!verified || verified.ssid !== joinSsid || verified.password !== joinPassword) {
        setJoinValidationStatus(t("wifiJoin.reverify"), "bad");
        return;
      }
      const useStatic = joinIpMode() === "static";
      if (useStatic) {
        const ip = document.getElementById("joinStaticIp").value.trim();
        const gw = document.getElementById("joinStaticGateway").value.trim();
        const mask = document.getElementById("joinStaticNetmask").value.trim();
        if (!ip || !isValidIpv4(ip)) {
          setJoinValidationStatus(t("network.staStaticIpInvalid"), "bad");
          return;
        }
        if (!gw || !isValidIpv4(gw)) {
          setJoinValidationStatus(t("network.staStaticGatewayInvalid"), "bad");
          return;
        }
        if (!mask || !isValidIpv4(mask)) {
          setJoinValidationStatus(t("network.staStaticNetmaskInvalid"), "bad");
          return;
        }
      }
      const { payload } = buildNetworkPayload();
      payload.sta_ssid = joinSsid;
      payload.sta_password = joinPassword;
      delete payload.sta_password_clear;
      payload.sta_use_static = useStatic ? 1 : 0;
      if (useStatic) {
        payload.sta_static_ip = document.getElementById("joinStaticIp").value.trim();
        payload.sta_static_gateway = document.getElementById("joinStaticGateway").value.trim();
        payload.sta_static_netmask = document.getElementById("joinStaticNetmask").value.trim();
        const dns1 = document.getElementById("joinStaticDns1").value.trim();
        const dns2 = document.getElementById("joinStaticDns2").value.trim();
        if (dns1) {
          payload.sta_static_dns1 = dns1;
        }
        if (dns2) {
          payload.sta_static_dns2 = dns2;
        }
      }
      closeWifiJoinModal();
      await submitNetworkConfig(payload, "");
    }

    function mergeSavedWifiProfiles(profiles, current) {
      const merged = [];
      const source = Array.isArray(profiles) ? profiles : [];
      const currentProfile = current?.sta_ssid
        ? { ssid: current.sta_ssid, password_set: Boolean(current.sta_password_set), current: true }
        : null;

      if (currentProfile) {
        merged.push(currentProfile);
      }
      source.forEach((profile) => {
        if (!profile?.ssid) {
          return;
        }
        const existing = merged.find((item) => item.ssid === profile.ssid);
        if (existing) {
          if (!existing.password_set && profile.password_set) {
            existing.password_set = true;
          }
          return;
        }
        merged.push({
          ssid: profile.ssid,
          password_set: Boolean(profile.password_set),
          current: false
        });
      });
      return merged;
    }

    function renderSavedWifiProfiles(profiles) {
      const target = document.getElementById("savedWifiList");
      const current = state.networkConfig || {};
      const items = mergeSavedWifiProfiles(profiles, current);

      state.savedWifiProfiles = items;
      if (!items.length) {
        target.innerHTML = `<div class="scan-item"><span>${t("scan.savedEmptyTitle")}</span><span class="hint">${t("scan.savedEmptyHint")}</span></div>`;
        return;
      }
      target.innerHTML = items.map((profile, index) => {
        const maskedPassword = profile.password_set ? "********" : t("network.none");
        const isCurrent = Boolean(profile.current);
        const hintText = isCurrent
          ? `${t("network.profileCurrent")} / ${maskedPassword}`
          : (current.sta_connected && current.sta_ssid === profile.ssid
              ? `${t("state.connected")} / ${maskedPassword}`
              : maskedPassword);
        /* 当前使用的 STA 配置由主配置(sta_ssid)承载,不属于已保存 profile 列表:
         * 删除它没有意义(服务端会 404 或删完又被重新合并显示),故不给出删除按钮 */
        const deleteButton = isCurrent
          ? ""
          : `<button class="secondary" data-saved-delete="${index}">${t("button.delete")}</button>`;
        return `
        <div class="scan-item">
          <div>
            <strong>${escapeHtml(profile.ssid || t("scan.hidden"))}</strong>
            <div class="hint">${hintText}</div>
          </div>
          <div class="actions">
            <button class="secondary" data-saved-use="${index}">${t("button.use")}</button>
            ${deleteButton}
          </div>
        </div>`;
      }).join("");

      target.querySelectorAll("button[data-saved-use]").forEach((button) => {
        button.addEventListener("click", () => applySavedWifiProfile(Number(button.dataset.savedUse)));
      });
      target.querySelectorAll("button[data-saved-delete]").forEach((button) => {
        button.addEventListener("click", async () => {
          const profile = state.savedWifiProfiles[Number(button.dataset.savedDelete)];
          if (!profile) {
            return;
          }
          await deleteSavedWifiProfile(profile.ssid);
        });
      });
    }

    function renderScanResults(aps) {
      const target = document.getElementById("wifiScanList");
      const items = Array.isArray(aps) ? aps : [];

      state.wifiScanResults = items;
      if (!items.length) {
        target.innerHTML = `<div class="scan-item"><span>${t("scan.emptyTitle")}</span><span class="hint">${t("scan.emptyHint")}</span></div>`;
        return;
      }
      target.innerHTML = items.map((ap, index) => {
        const ssid = ap.ssid || "";
        const label = ssid || t("scan.hidden");
        return `<div class="scan-item">
          <div>
            <strong>${escapeHtml(label)}</strong>
            <div class="hint">RSSI ${ap.rssi} dBm / ${ap.auth}${ap.bssid ? " / " + escapeHtml(ap.bssid) : ""}</div>
          </div>
          <button class="secondary" data-scan-use="${index}" ${ssid ? "" : "disabled"}>${t("button.use")}</button>
        </div>`;
      }).join("");

      target.querySelectorAll("button[data-scan-use]").forEach((button) => {
        button.addEventListener("click", () => applyScannedWifi(Number(button.dataset.scanUse)));
      });
    }

    function tryParseJson(text) {
      if (!text) {
        return { body: null, parseError: null };
      }

      const sanitized = text.replace(/^\uFEFF/, "").trim();
      if (!sanitized) {
        return { body: null, parseError: null };
      }

      const parseCandidates = [sanitized];
      const cleaned = sanitized.replace(/[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F]/g, "");
      if (cleaned !== sanitized) {
        parseCandidates.push(cleaned);
      }

      for (const candidateText of parseCandidates) {
        try {
          return { body: JSON.parse(candidateText), parseError: null };
        } catch (error) {
          const firstObject = candidateText.indexOf("{");
          const lastObject = candidateText.lastIndexOf("}");
          const firstArray = candidateText.indexOf("[");
          const lastArray = candidateText.lastIndexOf("]");
          let candidate = "";

          if (firstObject !== -1 && lastObject > firstObject) {
            candidate = candidateText.slice(firstObject, lastObject + 1);
          } else if (firstArray !== -1 && lastArray > firstArray) {
            candidate = candidateText.slice(firstArray, lastArray + 1);
          }

          if (candidate) {
            try {
              return { body: JSON.parse(candidate), parseError: null };
            } catch (candidateError) {
              // Continue trying the next sanitized candidate before reporting failure.
            }
          }
        }
      }

      return { body: { message: sanitized || "invalid_response" }, parseError: new Error("invalid_response") };
    }

    /* 全局单飞队列(1.3-36):设备 httpd 只开少量连接槽 + lru 踢旧,
     * 浏览器并行 fetch(面板两批加载、状态轮询)会互相顶掉**正在发响应**的
     * 连接,表现为"登录后部分模块一直加载/报错"。所有 API 请求经此串行,
     * 复用同一条 keep-alive 连接:局域网内首屏 7 个请求合计约 0.5s,
     * 且再也不会与手机门户探测等第二客户端争槽位。 */
