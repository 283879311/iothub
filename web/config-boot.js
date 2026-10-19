    async function getOtaRuntimeSnapshot() {
      const [info, status] = await Promise.all([
        getJson("/api/v1/device/info"),
        getJson("/api/v1/device/status")
      ]);
      return {
        appVersion: info.app_version || "",
        compileDate: info.compile_date || "-",
        compileTime: info.compile_time || "-",
        compileMarker: `${info.app_version || ""}|${info.compile_date || ""}|${info.compile_time || ""}`,
        partition: (status.app_partition && status.app_partition.label) || "unknown"
      };
    }

    async function waitForOtaRecovery(kind, baselineSnapshot) {
      const startedAt = Date.now();
      const timeoutMs = 120000;
      const pollMs = 2000;
      let sawOffline = false;

      setStatus("otaStatus", t("ota.waitingForReboot"), "warn");

      while (Date.now() - startedAt < timeoutMs) {
        await delay(pollMs);
        try {
          const snapshot = await getOtaRuntimeSnapshot();
          const compileChanged = !baselineSnapshot || snapshot.compileMarker !== baselineSnapshot.compileMarker;
          const partitionChanged = !!baselineSnapshot && snapshot.partition !== baselineSnapshot.partition;
          const elapsedMs = Date.now() - startedAt;

          if (kind === "firmware") {
            if (compileChanged || partitionChanged) {
              setStatus("otaStatus", t("ota.firmwareVerified", {
                partition: snapshot.partition,
                compileDate: snapshot.compileDate,
                compileTime: snapshot.compileTime
              }), "ok");
              return;
            }
            setStatus("otaStatus", t("ota.waitingForFirmware", {
              partition: snapshot.partition
            }), "warn");
            continue;
          }

          if (sawOffline || elapsedMs >= 8000) {
            setStatus("otaStatus", t("ota.storageVerified"), "ok");
            return;
          }
          setStatus("otaStatus", t("ota.waitingForStorage"), "warn");
        } catch (error) {
          sawOffline = true;
          setStatus("otaStatus", t("ota.waitingForReboot"), "warn");
        }
      }

      let partition = (baselineSnapshot && baselineSnapshot.partition) || "unknown";
      try {
        const snapshot = await getOtaRuntimeSnapshot();
        partition = snapshot.partition || partition;
      } catch (error) {
        // Keep the last known partition when the device is still offline.
      }
      throw new Error(t("ota.verificationTimedOut", { partition }));
    }

    function ensureExpectedUploadName(file, expectedName, missingKey, mismatchKey) {
      if (!file) {
        throw new Error(t(missingKey));
      }
      if (file.name !== expectedName) {
        throw new Error(t(mismatchKey, { actual: file.name || "-" }));
      }
    }

    async function uploadBinary(fileInputId, endpoint, missingKey, mismatchKey, expectedName, uploadingKey) {
      const file = document.getElementById(fileInputId).files[0];
      ensureExpectedUploadName(file, expectedName, missingKey, mismatchKey);
      const baselineSnapshot = await getOtaRuntimeSnapshot().catch(() => null);

      setStatus("otaStatus", t(uploadingKey, { name: file.name, size: file.size }), "warn");
      const data = await getJson(endpoint, {
        method: "POST",
        headers: { "Content-Type": "application/octet-stream" },
        body: file
      });
      setStatus("otaStatus", t("ota.done", { message: data.message }), "ok");
      await waitForOtaRecovery(endpoint === "/api/v1/ota/upload" ? "firmware" : "storage", baselineSnapshot);
    }

    async function uploadOta() {
      const allowDowngrade = document.getElementById("otaAllowDowngrade").checked;
      const endpoint = allowDowngrade ? "/api/v1/ota/upload?allow_downgrade=1" : "/api/v1/ota/upload";
      await uploadBinary("otaFile", endpoint, "error.selectFirmware", "error.expectedFirmwareName", "iothub.bin", "ota.uploading");
    }

    async function uploadStorage() {
      await uploadBinary("storageFile", "/api/v1/storage/upload", "error.selectStorage", "error.expectedStorageName", "storage.bin", "ota.storageUploading");
    }

    const PANEL_LOADERS = {
      device: loadDevice,
      network: loadNetwork,
      mqtt: loadMqtt,
      bluetooth: loadBluetooth,
      gpio: loadGpio,
      uart: async () => {
        await loadUart();
        await refreshUartConsole();
      }
    };
    const PANEL_ERROR_STATUS = {
      network: "networkStatus",
      mqtt: "mqttStatus",
      bluetooth: "bluetoothStatus",
      gpio: "gpioInfo",
      uart: "uartStatus"
    };

    state.activePanel = localStorage.getItem("iothub_panel") || "network";
    state.loadedPanels = state.loadedPanels || {};

    function showActivePanel(name) {
      document.querySelectorAll("[data-panel-card]").forEach((card) => {
        card.hidden = card.dataset.panelCard !== name;
      });
      document.querySelectorAll("#configNav [data-panel]").forEach((button) => {
        const active = button.dataset.panel === name;
        button.classList.toggle("active", active);
        if (active) {
          button.setAttribute("aria-current", "page");
        } else {
          button.removeAttribute("aria-current");
        }
      });
    }

    async function activatePanel(name, force = false) {
      if (!PANEL_LOADERS[name] && name !== "webAuth" && name !== "ota" && name !== "device") {
        name = "network";
      }
      state.activePanel = name;
      localStorage.setItem("iothub_panel", name);
      showActivePanel(name);

      if (name === "uart") {
        startUartConsolePolling();
      } else if (state.uartPollTimer) {
        clearInterval(state.uartPollTimer);
        state.uartPollTimer = null;
      }
      if (name === "gpio") {
        startGpioPolling();
      } else {
        stopGpioPolling();
      }

      if (name === "webAuth" || name === "ota") {
        return;
      }
      if (!force && state.loadedPanels[name]) {
        return;
      }
      const loader = PANEL_LOADERS[name];
      if (!loader) {
        return;
      }
      try {
        await loader();
        state.loadedPanels[name] = true;
      } catch (error) {
        const statusId = PANEL_ERROR_STATUS[name];
        const message = getErrorMessage(error);
        if (statusId) {
          setStatus(statusId, message, "bad");
        } else {
          renderDeviceError(message);
        }
      }
    }

    async function boot() {
      try {
        await ensureTranslations(state.language);
      } catch (error) {
        state.language = "zh-CN";
        localStorage.setItem("iothub_lang", state.language);
      }
      renderUartConsole();
      applyTranslations();
      await activatePanel(state.activePanel, true);
      startStatusPolling();
    }

    document.querySelectorAll("#configNav [data-panel]").forEach((button) => {
      button.addEventListener("click", () => {
        activatePanel(button.dataset.panel).catch((error) => {
          setStatus("networkStatus", getErrorMessage(error), "bad");
        });
      });
    });

    document.getElementById("languageSelect").addEventListener("change", async (event) => {
      state.language = event.target.value;
      localStorage.setItem("iothub_lang", state.language);
      await boot();
    });

    document.getElementById("saveNetwork").addEventListener("click", async () => {
      try {
        await saveNetwork();
      } catch (error) {
        setNetworkValidationStatus(error.message, "bad");
      }
    });

    ["netMode", "ssid", "password", "apPasswordClear", "staSsid", "staPassword", "staPasswordClear"].forEach((id) => {
      const el = document.getElementById(id);
      if (!el) {
        return;
      }
      const isToggle = id === "netMode" || id.endsWith("Clear");
      el.addEventListener(isToggle ? "change" : "input", () => {
        updateNetworkValidationStatus();
      });
    });

    document.getElementById("scanWifi").addEventListener("click", async () => {
      try {
        await scanNetwork();
      } catch (error) {
        setStatus("networkStatus", error.message, "bad");
      }
    });

    document.getElementById("showSavedWifi").addEventListener("click", () => {
      renderSavedWifiProfiles(state.savedWifiProfiles);
      openSavedWifiModal();
    });

    document.getElementById("closeWifiScanModal").addEventListener("click", () => {
      closeWifiScanModal();
    });

    document.getElementById("wifiScanModal").addEventListener("click", (event) => {
      if (event.target.id === "wifiScanModal") {
        closeWifiScanModal();
      }
    });

    /* 连接 Wi-Fi 向导(1.3-51):验证密码 / 保存 / 取消 / 静态面板开关 */
    document.getElementById("joinVerify").addEventListener("click", async () => {
      await verifyJoinWifi();
    });

    document.getElementById("joinSave").addEventListener("click", async () => {
      await saveJoinWifi();
    });

    document.getElementById("joinCancel").addEventListener("click", () => {
      closeWifiJoinModal();
    });

    document.getElementById("wifiJoinModal").addEventListener("click", (event) => {
      if (event.target.id === "wifiJoinModal") {
        closeWifiJoinModal();
      }
    });

    document.querySelectorAll("input[name=\"joinIpMode\"]").forEach((input) => {
      input.addEventListener("change", updateJoinStaticPanel);
    });

    document.getElementById("closeSavedWifiModal").addEventListener("click", () => {
      closeSavedWifiModal();
    });

    document.getElementById("ipConflictDismiss").addEventListener("click", () => {
      closeIpConflictModal();
    });

    document.getElementById("ipConflictGoFix").addEventListener("click", () => {
      /* 1.3-52:静态 IP 已移入连接向导 —— 冲突修复同样指到向导,
       * 并预选"静态 IP"、回显冲突地址方便直接改。 */
      closeIpConflictModal();
      openWifiJoinModal((state.networkConfig?.sta_ssid) || "");
      document.getElementById("joinIpStatic").checked = true;
      updateJoinStaticPanel();
      const conflictedIp = state.networkConfig?.sta_static_ip;
      if (conflictedIp) {
        const el = document.getElementById("joinStaticIp");
        el.value = conflictedIp;
        el.classList.add("input-error");
        el.focus();
      }
    });

    document.getElementById("savedWifiModal").addEventListener("click", (event) => {
      if (event.target.id === "savedWifiModal") {
        closeSavedWifiModal();
      }
    });

    document.getElementById("toggleRelay").addEventListener("click", async () => {
      try {
        await sendGpioCommand("relay.toggle");
      } catch (error) {
        setStatus("gpioInfo", error.message, "bad");
      }
    });

    document.getElementById("toggleLed").addEventListener("click", async () => {
      try {
        await sendGpioCommand("led.toggle");
      } catch (error) {
        setStatus("gpioInfo", error.message, "bad");
      }
    });

    document.getElementById("saveGpioPolicy").addEventListener("click", async () => {
      try {
        await saveGpioPolicy();
        setStatus("gpioInfo", t("gpio.policySaved"), "ok");
      } catch (error) {
        setStatus("gpioInfo", error.message, "bad");
      }
    });

    document.getElementById("saveWebAuth").addEventListener("click", async () => {
      try {
        await saveWebAuth();
      } catch (error) {
        setStatus("webAuthStatus", getErrorMessage(error), "bad");
      }
    });

    document.getElementById("saveBluetooth").addEventListener("click", async () => {
      try {
        await saveBluetooth();
      } catch (error) {
        setStatus("bluetoothStatus", error.message, "bad");
      }
    });

    document.getElementById("uploadOta").addEventListener("click", async () => {
      try {
        await uploadOta();
      } catch (error) {
        setStatus("otaStatus", error.message, "bad");
      }
    });

    document.getElementById("uploadStorage").addEventListener("click", async () => {
      try {
        await uploadStorage();
      } catch (error) {
        setStatus("otaStatus", error.message, "bad");
      }
    });

    document.getElementById("saveMqtt").addEventListener("click", async () => {
      try {
        await saveMqtt();
      } catch (error) {
        setStatus("mqttStatus", error.message, "bad");
      }
    });

    document.getElementById("saveUart").addEventListener("click", async () => {
      try {
        await saveUart();
      } catch (error) {
        setStatus("uartStatus", error.message, "bad");
      }
    });

    document.getElementById("uartRxFormat").addEventListener("change", (event) => {
      state.uartRxFormat = event.target.value;
      renderUartConsole();
    });

    document.getElementById("uartTxFormat").addEventListener("change", (event) => {
      state.uartTxFormat = event.target.value;
      renderUartConsole();
    });

    document.getElementById("sendUart").addEventListener("click", async () => {
      try {
        await sendUart();
      } catch (error) {
        setStatus("uartStatus", error.message, "bad");
      }
    });

    document.getElementById("clearUartConsole").addEventListener("click", async () => {
      try {
        await clearUartConsole();
      } catch (error) {
        setStatus("uartStatus", error.message, "bad");
      }
    });

    applyTranslations();
    boot();
