# [OPEN] uart-console-blank

## Symptoms
- Web `config` page UART receive area stays blank.
- Frontend sometimes reports truncated `HTTP 200` body like `{"plain":"`.
- External serial assistant shows loopback-like receive of sent text such as `9922aa`.

## Expected
- `/api/v1/uart/console` returns complete JSON reliably.
- Web receive area shows buffered UART data in plain/hex modes.

## Hypotheses
1. `/api/v1/uart/console` response is truncated during chunked send, so frontend parse fails and keeps the textarea blank.
2. UART console buffer is being updated, but frontend polling/render path is receiving invalid or partial payloads and silently preserving empty state.
3. UART console buffer is not updated consistently because runtime polling and HTTP-triggered polling race or miss incoming bytes.
4. The observed serial assistant "loopback" is external to the web console bug; the web issue is isolated to HTTP response integrity rather than UART RX hardware.

## Plan
1. Instrument backend console buffer state and HTTP response send progress.
2. Instrument frontend polling/parsing/render checkpoints.
3. Reproduce once and compare evidence across backend and frontend.
4. Apply minimal fix only after evidence confirms the failing stage.

## Evidence
- Frontend logs show `/api/v1/config/uart` parses successfully and reports `driver_installed=true`.
- Frontend logs show `/api/v1/uart/console` now returns complete JSON consistently: `{"plain":"","hex":"","rx_sequence":0}`.
- Frontend logs show `parseError=false` for `/api/v1/uart/console`; the response is valid but empty.
- Frontend render logs show `renderedLength=0`, matching the empty backend payload.

## Interim Analysis
- Hypothesis 1 is rejected for the latest reproduction: no truncated JSON was observed in the captured run.
- Hypothesis 2 is rejected for the latest reproduction: frontend parsing/rendering is functioning, but the payload is empty.
- Hypothesis 3 remains plausible: backend console buffer is not being populated.
- Hypothesis 4 remains plausible: serial assistant loopback may be independent of the empty web console.

## Latest Runtime Evidence
- Backend reports `driver_installed=true` on `/api/v1/uart/console`.
- Backend polling task is alive: `debug_poll_count` keeps increasing across requests.
- Backend never sees buffered input: `debug_available=0` and `debug_read_len=0` for every sampled poll.
- Therefore, the UART driver buffer for the configured port receives no bytes during reproduction.

## Updated Analysis
- Hypothesis 3 is confirmed in refined form: the backend console buffer stays empty because the configured UART port sees zero incoming bytes.
- The remaining uncertainty is external to the web console pipeline: wrong UART port, wrong wiring, local echo/loopback in the serial assistant, or traffic going to a different UART than `UART1 GPIO26/GPIO27`.

## निर्णायक Evidence
- User confirmed `SSCOM <- ESP(GPIO27 TX)` works.
- User confirmed with `GPIO27(TX) -> GPIO26(RX)` local jumper loopback, the web console successfully receives `9922aa`.
- Therefore the firmware UART stack, `console` buffer, HTTP API, and frontend rendering path are all functioning end-to-end.

## Root Cause
- The failure is isolated to the external `SSCOM TX -> ESP GPIO26(RX)` ingress path.
- This is most consistent with one-way wiring/adapter issues, voltage-level mismatch, bad TX line on the USB-UART adapter, missing common ground on the receive path, or external line contention/noise before bytes reach `UART1`.
