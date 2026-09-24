# EspUsbHost (vendored, modified)

Based on [EspUsbHost](https://github.com/tanakamasayuki/EspUsbHost) 1.0.1 by TANAKA Masayuki
(MIT License). This copy is **modified** for this project: among other things it exposes
`isReady`, `usbTransferSize` and the `usbTransfer[]` pointer array that
`esp32_host.ino` uses to queue extra mouse URBs. The stock 1.0.1 from the Arduino library
manager does **not** compile with the sketch, so it lives here and the sketch includes it
by path.
