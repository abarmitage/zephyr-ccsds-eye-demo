# zephyr-ccsds EYE demo

Standalone two-board ESP32-S3-EYE camera/CFDP demo. The current milestone
provides the LVGL interface, role configuration, and button diagnostics; see
[PLAN.md](PLAN.md).

```sh
west build -b esp32s3_eye/esp32s3/procpu . -- -DEXTRA_CONF_FILE=conf/role-a.conf
```

Use `conf/role-b.conf` for the second board.

