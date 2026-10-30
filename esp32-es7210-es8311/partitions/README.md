# Partition table (`partitions.csv`)

Single **factory** application, **no OTA** slots, **no SPIFFS / assets** partition.

| Partition | Offset   | Size    | Notes                          |
|-----------|----------|---------|--------------------------------|
| nvs       | 0x9000   | 24 KB   | NVS (Wi‑Fi calibration, etc.)  |
| phy_init  | 0xF000   | 4 KB    | PHY init data                  |
| factory   | 0x10000  | ~3.94 MB| Application (`factory` image)  |

Total span ends at **4 MB** (`0x400000`), so this CSV is safe on **4 MB Flash** modules.

On **larger Flash**, either keep this layout (unused tail) or increase the `factory` `Size` and set **menuconfig → Serial flasher → Flash size** to match your chip.
