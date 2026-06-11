# Connecting to OpenPineBuds via Bluetooth Serial (RFCOMM)

The firmware exposes **two independent SPP (Serial Port Profile) channels**:

| Channel | Wire # | Purpose | Device node (suggested) |
|---|---|---|---|
| `RFCOMM_CHANNEL_GREEN` | 15 | Data / raw mic stream | `/dev/rfcomm1` |
| `RFCOMM_CHANNEL_RED`   | 16 | Remote logging        | `/dev/rfcomm2` |

MAC address: `20:22:11:12:34:6F`

---

## Prerequisites

```bash
sudo apt install bluez rfcomm minicom
```

---

## Quick Start (Automated Script)

A helper script is included in the repo root and connects the data channel:

```bash
sudo ./connect_serial.sh
```

---

## Manual Steps

### 1. Pair the device

If not already paired:

```bash
bluetoothctl
> scan on
> pair 20:22:11:12:34:6F
> trust 20:22:11:12:34:6F
> quit
```

### 2. Bind both RFCOMM channels

```bash
sudo rfcomm bind rfcomm1 20:22:11:12:34:6F 15   # data channel
sudo rfcomm bind rfcomm2 20:22:11:12:34:6F 16   # log channel
```

You can bind only the channel(s) you need. Any device number (rfcomm0, rfcomm1,
...) is fine as long as it is not already in use.

### 3. Connect to the data channel

**minicom:**

```bash
minicom -D /dev/rfcomm1
```

**screen:**

```bash
screen /dev/rfcomm1
```

**Python (pyserial):**

```python
import serial
ser = serial.Serial('/dev/rfcomm1', timeout=1)
ser.write(b'hello\n')
print(ser.read(64))
ser.close()
```

On a successful connection the device sends:

```
Hello from OpenPineBuds!
```

Any data sent to the data channel is echoed back (loopback test mode).

### 4. Read the remote log channel

```bash
minicom -D /dev/rfcomm2
# or
cat /dev/rfcomm2
```

The device sends structured log lines over this channel whenever
`app_spp_log_printf(...)` is called in firmware. The channel is read-only from
the device's perspective — data sent from the PC is silently ignored.

### 5. Capture raw binary data (for mic streaming)

To dump the raw byte stream to a file:

```bash
cat /dev/rfcomm1 > capture.raw
```

Stop with `Ctrl+C`. Import into Audacity via **File > Import > Raw Data**:
- Encoding: Signed 16-bit PCM
- Byte order: Little-endian
- Channels: 1 (Mono)
- Sample rate: 16000 Hz

### 6. Release bindings when done

```bash
sudo rfcomm release rfcomm1
sudo rfcomm release rfcomm2
```

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `rfcomm bind` hangs | Device not in range / not advertising | Make sure earbuds are powered on and BT is active |
| `Can't open RFCOMM device` | Wrong channel number | Data = **15**, Log = **16** |
| `/dev/rfcommN` exists but no data | Connection dropped | Release and re-bind |
| `Permission denied` on `/dev/rfcommN` | Not running as root | Use `sudo` or `sudo usermod -aG dialout $USER` |
| minicom shows garbage / no echo | Line settings mismatch | RFCOMM is a raw stream; baud rate in minicom is irrelevant — ignore it |
| Log channel connects but is silent | No firmware event triggered yet | Normal — the log channel only transmits when `app_spp_log_printf` is called |

---

## Using the log channel from firmware

```c
#include "app_spp_log.h"

// Plain string
app_spp_log_printf("[mymodule] started, value=%d\r\n", some_val);

// Check if anyone is listening before doing expensive formatting
if (app_spp_log_is_connected()) {
    app_spp_log_printf("[sensor] raw=%d filtered=%d\r\n", raw, filtered);
}
```

---

## Reference

| Parameter | Value |
|---|---|
| MAC Address | `20:22:11:12:34:6F` |
| Data channel (wire) | 15 (`RFCOMM_CHANNEL_GREEN`) |
| Log channel (wire) | 16 (`RFCOMM_CHANNEL_RED`) |
| SPP UUID (both) | `00001101-0000-1000-8000-00805F9B34FB` |
| Data device node | `/dev/rfcomm1` |
| Log device node | `/dev/rfcomm2` |
