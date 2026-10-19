    async function loadGpio() {
      const data = await getJson("/api/v1/config/gpio");
      state.relayOn = Boolean(data.relay_on);
      state.ledOn = Boolean(data.led_on);
      state.gpioConfig = data;
      document.getElementById("relayGpioDisplay").textContent = formatGpioLabel(data.relay_gpio);
      document.getElementById("ledGpioDisplay").textContent = formatGpioLabel(data.led_gpio);
      document.getElementById("inputGpioDisplay").textContent = formatGpioLabel(data.input_gpio);
      renderGpioStatus(data);
      document.getElementById("relayPowerPolicy").value = String(data.relay_power_policy ?? 0);
      document.getElementById("ledPowerPolicy").value = String(data.led_power_policy ?? 0);
      setStatus("gpioInfo", t("gpio.summary", {
        relayGpio: data.relay_gpio,
        relayOn: data.relay_on ? t("state.on") : t("state.off"),
        ledGpio: data.led_gpio,
        ledOn: data.led_on ? t("state.on") : t("state.off"),
        inputGpio: data.input_gpio
      }), "ok");
    }

    function renderGpioStatus(gpio) {
      if (!gpio) {
        return;
      }
      if (typeof gpio.input_active === "boolean") {
        const input = document.getElementById("inputState");
        const active = Boolean(gpio.input_active);
        input.textContent = t(active ? "input.pressed" : "input.released");
        input.classList.toggle("input-active", active);
      }
      if (typeof gpio.relay_on === "boolean") {
        state.relayOn = Boolean(gpio.relay_on);
      }
      if (typeof gpio.led_on === "boolean") {
        state.ledOn = Boolean(gpio.led_on);
      }
    }

    async function refreshGpioStatus() {
      if (state.gpioPollBusy) {
        return;
      }
      state.gpioPollBusy = true;
      try {
        renderGpioStatus(await getJson("/api/v1/config/gpio"));
      } finally {
        state.gpioPollBusy = false;
      }
    }

    function startGpioPolling() {
      if (state.gpioPollTimer) {
        clearInterval(state.gpioPollTimer);
      }
      state.gpioPollTimer = setInterval(() => {
        if (document.hidden) {
          return;
        }
        refreshGpioStatus().catch(() => {});
      }, 1000);
    }

    function stopGpioPolling() {
      if (state.gpioPollTimer) {
        clearInterval(state.gpioPollTimer);
        state.gpioPollTimer = null;
      }
    }

    async function loadBluetooth() {
      const data = await getJson("/api/v1/config/bluetooth");
      const bluetooth = data || {};
      document.getElementById("btMode").value = bluetooth.mode || "off";
      document.getElementById("btName").value = bluetooth.device_name || "";
      renderBluetoothStatus(bluetooth);
    }

    function renderBluetoothStatus(data) {
      const bluetooth = data || {};
      const hasError = Boolean(bluetooth.last_error);
      setStatus("bluetoothStatus", t(hasError ? "bluetooth.summaryWithError" : "bluetooth.summary", {
        mode: humanBtMode(bluetooth.mode),
        runtimeMode: humanBtMode(bluetooth.runtime_mode),
        name: bluetooth.device_name || t("network.none"),
        error: bluetooth.last_error || t("network.none")
      }), hasError ? "warn" : "ok");
    }

    async function loadMqtt() {
      const data = await getJson("/api/v1/config/mqtt");
      document.getElementById("mqttBackend").value = data.backend || "tb_cloud";
      document.getElementById("mqttHost").value = data.host || "";
      document.getElementById("mqttPort").value = data.port || 1883;
      document.getElementById("mqttToken").value = "";
      document.getElementById("mqttTokenClear").checked = false;
      document.getElementById("mqttTls").checked = Boolean(data.use_tls);
      renderMqttStatus(data);
    }

    function renderMqttStatus(data) {
      setStatus("mqttStatus", t("mqtt.summary", {
        backend: data.backend,
        host: data.host,
        port: data.port,
        connected: data.connected ? t("state.connected") : t("state.disconnected"),
        error: data.last_error || t("network.none")
      }), data.connected ? "ok" : "warn");
    }

    async function refreshStatusPanels() {
      if (state.statusPollBusy) {
        return;
      }
      state.statusPollBusy = true;
      try {
        const deviceData = await getJson("/api/v1/device/status");
        renderDevice(deviceData);
        renderNetworkStatus(deviceData?.network || {});
        renderBluetoothStatus(deviceData?.bluetooth || {});
        renderMqttStatus(deviceData?.mqtt || {});
        renderGpioStatus(deviceData?.gpio || {});
      } catch (error) {
        renderDeviceError(getErrorMessage(error));
      } finally {
        state.statusPollBusy = false;
      }
    }

    /* 轮询连续失败计数:会话失效或设备过载时**主动停轮询**。两个浏览器同开时,
     * 旧标签页的轮询会把正在使用的新页面挤成"加载中"(设备 httpd 可用连接有限)。
     * 见 问题与版本优化.md 1.3-22 */
    state.pollFailCount = 0;
    state.pollStopped = false;
    function notePollOk() {
      state.pollFailCount = 0;
      if (!state.pollStopped) {
        return;
      }
      /* 自愈:被停过之后,只要有请求成功(用户点按钮/刷新入数据)就恢复轮询 */
      state.pollStopped = false;
      refreshStatusPanels().catch(() => {});
      if (state.activePanel === "uart") {
        refreshUartConsole().catch(() => {});
      }
      if (state.activePanel === "gpio") {
        refreshGpioStatus().catch(() => {});
      }
      startStatusPolling();
      if (state.activePanel === "uart") {
        startUartConsolePolling();
      }
      if (state.activePanel === "gpio") {
        startGpioPolling();
      }
    }

    function notePollFail(error) {
      if (error && error.status === 401) {
        window.location.href = "/portal";
        return;
      }
      state.pollFailCount += 1;
      if (state.pollFailCount >= 5 && !state.pollStopped) {
        state.pollStopped = true;
        stopPolling();
      }
    }

    /* 1.3-72:真实页面轮询收敛。状态 5s→10s;UART 控制台只在可见时拉取,
     * 滚出视野/后台标签页不请求 —— 降低页面多连接对 httpd/堆的压力。
     * 页面仍是单文件(构建期 minify+gzip),不拆 JS/CSS:当前 httpd 只缓存
     * 一个页面文件句柄,拆成多文件会让每次页面加载反复 open/close,反而增加
     * 低堆下的分配抖动;这里按“面板/请求”拆资源,而不是拆静态文件。 */
    const PAGE_POLL_INTERVAL_MS = 10000;

    function uartConsoleInView() {
      const el = document.getElementById("uartRxConsole");
      if (!el || el.offsetParent === null) {
        return false;
      }
      const rect = el.getBoundingClientRect();
      const vh = window.innerHeight || document.documentElement.clientHeight || 0;
      return rect.bottom > 0 && rect.top < vh;
    }

    function startStatusPolling() {
      if (state.statusPollTimer) {
        clearInterval(state.statusPollTimer);
      }
      state.pollStopped = false;
      state.pollFailCount = 0;
      state.statusPollTimer = setInterval(() => {
        if (document.hidden) {
          return;
        }
        refreshStatusPanels().catch(notePollFail);
      }, PAGE_POLL_INTERVAL_MS);
    }

    async function loadUart() {
      const data = await getJson("/api/v1/config/uart");
      state.uartConfig = data;
      document.getElementById("uartRxDisplay").textContent = formatGpioLabel(data.rx_gpio);
      document.getElementById("uartTxDisplay").textContent = formatGpioLabel(data.tx_gpio);
      renderUartBaudList();
      document.getElementById("uartBaudSelect").value = String(data.baudrate || 9600);
      document.getElementById("uartDataBits").value = String(data.data_bits || 8);
      document.getElementById("uartStopBits").value = String(data.stop_bits || 1);
      document.getElementById("uartParity").value = data.parity || "none";
      setStatus("uartStatus", t("uart.summary", {
        frame: data.frame,
        baudrate: data.baudrate
      }), "warn");
    }

    async function loadUartConsole() {
      const data = await getJson("/api/v1/uart/console");
      state.uartConsolePlain = data.plain || "";
      state.uartConsoleHex = data.hex || "";
      renderUartConsole();
    }

    async function refreshUartConsole() {
      if (state.uartPollBusy) {
        return;
      }
      state.uartPollBusy = true;
      try {
        await loadUartConsole();
      } finally {
        state.uartPollBusy = false;
      }
    }

    async function clearUartConsole() {
      await getJson("/api/v1/uart/console/clear", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: "{}"
      });
      state.uartConsolePlain = "";
      state.uartConsoleHex = "";
      renderUartConsole();
      setStatus("uartStatus", t("uart.cleared"), "ok");
    }

    function startUartConsolePolling() {
      if (state.uartPollTimer) {
        clearInterval(state.uartPollTimer);
      }
      state.pollStopped = false;
      state.pollFailCount = 0;
      state.uartPollTimer = setInterval(() => {
        if (document.hidden || !uartConsoleInView()) {
          return;
        }
        refreshUartConsole().catch((error) => {
          setStatus("uartStatus", getErrorMessage(error), "bad");
          notePollFail(error);
        });
      }, PAGE_POLL_INTERVAL_MS);
    }

    function stopPolling() {
      if (state.statusPollTimer) {
        clearInterval(state.statusPollTimer);
        state.statusPollTimer = null;
      }
      if (state.uartPollTimer) {
        clearInterval(state.uartPollTimer);
        state.uartPollTimer = null;
      }
      stopGpioPolling();
    }

    /* 页面不可见时停掉轮询:否则"开着旧标签"也会持续打设备(status 与 uart console 各每 5 秒一次),
     * 和当前正在刷新的页面抢 httpd 的 socket —— 而 httpd 可用 socket 仅 ~9 个,
     * 且**每个常驻连接都吃 httpd 会话缓冲**(1.3-24 实测:8 个僵死连接吃掉 ~6.6KB largest_free,
     * 叠加 BLE 连接态后历史最低堆只有 360B)。恢复可见时先立即刷一次。 */
    document.addEventListener("visibilitychange", () => {
      if (document.visibilityState === "hidden") {
        stopPolling();
        return;
      }
      refreshStatusPanels().catch(() => {});
      if (state.activePanel === "uart") {
        refreshUartConsole().catch(() => {});
      }
      startStatusPolling();
      if (state.activePanel === "uart") {
        startUartConsolePolling();
      }
    });

    /* 1.3-52 拆分:主表单与连接向导共用"组 payload + 提交"两段。
     * 主表单已不含静态 IP 控件(移到连接向导),保存时**原样保留**设备上已有
     * 的静态配置(从 state.networkConfig 重发),避免改 AP 名称时把静态 IP 弄丢。 */
    function buildNetworkPayload() {
      const netModeEl = document.getElementById("netMode");
      const staSsid = document.getElementById("staSsid").value.trim();
      let mode = netModeEl.value;
      let modeNote = "";
      /* 填了 STA 名称却仍是"纯 AP" → 上游永远连不上(1.3-04 实测):
       * 与后端统一层的一致性推断保持一致,这里先把界面切到 AP + STA 并提示用户 */
      if (staSsid && !networkModeHasSta(mode)) {
        netModeEl.value = "apsta";
        mode = "apsta";
        modeNote = t("network.modeAutoApsta");
      }
      const payload = {
        mode: mode,
        ap_ssid: document.getElementById("ssid").value.trim(),
        sta_ssid: staSsid
      };
      const apPassword = document.getElementById("password").value;
      if (apPassword) {
        payload.ap_password = apPassword;
      }
      if (document.getElementById("apPasswordClear").checked) {
        payload.ap_password_clear = true;
      }
      const staPassword = document.getElementById("staPassword").value;
      if (staPassword) {
        payload.sta_password = staPassword;
      }
      if (document.getElementById("staPasswordClear").checked) {
        payload.sta_password_clear = true;
      }
      const current = state.networkConfig || {};
      payload.sta_use_static = current.sta_use_static ? 1 : 0;
      if (current.sta_use_static) {
        payload.sta_static_ip = current.sta_static_ip || "";
        payload.sta_static_gateway = current.sta_static_gateway || "";
        payload.sta_static_netmask = current.sta_static_netmask || "";
        if (current.sta_static_dns1) {
          payload.sta_static_dns1 = current.sta_static_dns1;
        }
        if (current.sta_static_dns2) {
          payload.sta_static_dns2 = current.sta_static_dns2;
        }
      }
      return { payload, modeNote };
    }

    async function submitNetworkConfig(payload, modeNote) {
      const data = await getJson("/api/v1/config/network", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload)
      });
      await loadNetwork();
      setNetworkValidationStatus("", "");
      const address = data.next_access_ip ? `http://${data.next_access_ip}` : "";
      let note = modeNote || "";
      if (data.mode_adjusted) {
        note = t("network.modeAutoApsta");   /* 后端推断生效(例如模式被自动升级) */
      }
      const confirmMessage = `${note ? note + " " : ""}${data.ap_enabled
        ? t("network.savedNeedsRestart", { address })
        : t("network.savedStaOnly")}`;
      setStatus("networkStatus", confirmMessage, "warn");

      if (confirm(confirmMessage)) {
        const restartData = await getJson("/api/v1/network/restart", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: "{}"
        });
        const restartMessage = data.ap_enabled
          ? t("network.restartStarted", { address })
          : restartData.message;
        setStatus("networkStatus", restartMessage, "ok");
      }
    }

    async function saveNetwork() {
      const clientCheck = validateNetworkForm();
      if (!clientCheck.ok) {
        setNetworkValidationStatus(clientCheck.message, "bad");
        return;
      }
      setNetworkValidationStatus("", "");
      const { payload, modeNote } = buildNetworkPayload();
      await submitNetworkConfig(payload, modeNote);
    }

    async function deleteSavedWifiProfile(ssid) {
      try {
        await getJson("/api/v1/config/network/profile", {
          method: "DELETE",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ ssid })
        });
        setStatus("networkStatus", t("network.profileDeleted", { ssid }), "ok");
      } catch (error) {
        setStatus("networkStatus", t("network.profileDeleteFailed", { ssid }), "bad");
      }
      await loadNetwork();
    }

    async function scanNetwork() {
      setStatus("networkStatus", t("network.scanning"), "warn");
      const data = await getJson("/api/v1/network/scan");
      renderScanResults(data.aps || []);
      openWifiScanModal();
      setStatus("networkStatus", t("network.scanDone", { count: data.count || 0 }), "ok");
    }

    async function sendGpioCommand(method) {
      const data = await getJson("/api/v1/command", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ method, params: {} })
      });
      if (!data.ok) {
        throw new Error(data.error || "command_failed");
      }
      await loadGpio();
    }

    async function saveGpioPolicy() {
      await getJson("/api/v1/config/gpio", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          relay_power_policy: Number(document.getElementById("relayPowerPolicy").value),
          led_power_policy: Number(document.getElementById("ledPowerPolicy").value)
        })
      });
      await loadGpio();
    }

    async function saveBluetooth() {
      const payload = {
        mode: document.getElementById("btMode").value,
        device_name: document.getElementById("btName").value.trim()
      };
      const data = await getJson("/api/v1/config/bluetooth", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload)
      });
      if (data.restart_required) {
        /* 跨模式切换:设备将整机重启以释放/重选蓝牙栈内存 */
        setStatus("bluetoothStatus", t("bluetooth.savedNeedsReboot"), "warn");
        await waitForDeviceRestart();
        await loadBluetooth();
        return;
      }
      setStatus("bluetoothStatus", t("bluetooth.saving"), "warn");
      await new Promise((resolve) => setTimeout(resolve, 800));
      await loadBluetooth();
    }

    async function saveWebAuth() {
      const new_password = document.getElementById("webNewPassword").value;
      if (new_password.length < 8) {
        setStatus("webAuthStatus", t("web.tooShort"), "bad");
        return;
      }
      if (new_password !== document.getElementById("webNewPassword2").value) {
        setStatus("webAuthStatus", t("web.confirmMismatch"), "bad");
        return;
      }
      const payload = {
        old_password: document.getElementById("webOldPassword").value,
        new_password
      };
      const username = document.getElementById("webUsername").value.trim();
      if (username) {
        payload.new_username = username;
      }
      await getJson("/api/v1/config/web", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload)
      });
      setStatus("webAuthStatus", t("web.saved"), "warn");
      await new Promise((resolve) => setTimeout(resolve, 1500));
      location.reload();
    }

    async function waitForDeviceRestart() {
      for (let waited = 0; waited < 20000; waited += 2000) {
        await new Promise((resolve) => setTimeout(resolve, 2000));
        try {
          await getJson("/api/v1/device/info");
          return;
        } catch (error) {
          /* 设备重启中,继续等待 */
        }
      }
    }

    async function saveMqtt() {
      const payload = {
        backend: document.getElementById("mqttBackend").value,
        host: document.getElementById("mqttHost").value.trim(),
        port: Number(document.getElementById("mqttPort").value),
        use_tls: document.getElementById("mqttTls").checked
      };
      const token = document.getElementById("mqttToken").value.trim();
      if (token) {
        payload.token = token;
      }
      if (document.getElementById("mqttTokenClear").checked) {
        payload.token_clear = true;
      }
      const data = await getJson("/api/v1/config/mqtt", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload)
      });
      setStatus("mqttStatus", data.message, "warn");
    }

    async function saveUart() {
      const baudrate = Number(document.getElementById("uartBaudSelect").value);
      const payload = {
        baudrate,
        data_bits: Number(document.getElementById("uartDataBits").value),
        stop_bits: Number(document.getElementById("uartStopBits").value),
        parity: document.getElementById("uartParity").value
      };
      await getJson("/api/v1/config/uart", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload)
      });
      await loadUart();
      await loadGpio();
    }

    async function sendUart() {
      const payload = {
        encoding: state.uartTxFormat,
        data: document.getElementById("uartTxData").value
      };
      const data = await getJson("/api/v1/uart/send", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload)
      });
      setStatus("uartStatus", t("uart.sent", {
        bytes: data.bytes_sent || 0,
        encoding: state.uartTxFormat
      }), "ok");
      document.getElementById("uartTxData").value = "";
      await refreshUartConsole();
    }

    function delay(ms) {
      return new Promise((resolve) => window.setTimeout(resolve, ms));
    }
