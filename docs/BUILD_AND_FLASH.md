# 編譯、備份與燒錄

所有命令預設從專案根目錄執行；`COM3` 是目前電腦上的ESP32-S3，其他環境請換成 `arduino-cli board list` 顯示的連接埠。

## 工具鏈

| 工具 | 本專案設定 |
|---|---|
| Arduino CLI | 1.5.1 |
| PowerShell | 7以上（`pwsh`） |
| Espressif Arduino core | `esp32:esp32@3.3.11` |
| Python | Python 3，用於網頁嵌入及測試工具 |
| Node.js | 20以上，執行內建 `node:test` |
| 桌面 C++ | 支援C++17的 `g++`／`clang++`，或以 `CXX` 指定，執行可攜協定測試 |
| 目標硬體 | ESP32-S3、16 MB QIO Flash、8 MB OPI PSRAM |

版本／FQBN的唯一來源為 [`toolchain.json`](../toolchain.json)。使用 Arduino core 內附的BLE及USB介面，不安裝額外第三方BLE函式庫。由於直接使用內附NimBLE API，升級core必須重新做編譯與BLE／USB實測。

```powershell
arduino-cli version
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.11 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core list
```

Arduino CLI可由 [官方版本頁](https://github.com/arduino/arduino-cli/releases/tag/v1.5.1) 取得。PowerShell腳本依序使用顯式 `-ArduinoCli`、PATH、Arduino IDE內附工具；使用後兩者時自行核對版本。Python可傳 `-Python` 指定。不要用隱含的其他core版本取代固定版本。

## 本機測試

```powershell
python tools/run_tests.py
node --test tests/frontend.test.cjs
```

第一個命令執行專案的桌面測試流程；第二個命令適合只修改前端時快速驗證。測試使用人工建立的封包、MAC、UID及讀值，不需要連接實體裝置。不把通過的模擬測試當成物理USB／無線電／機台驗收。

可用 `python tools/run_tests.py --cpp-only` 只測可攜C++，支援的環境可加 `--sanitize` 進行Address／Undefined Behavior sanitizer檢查。Windows沒有桌面編譯器時可在WSL執行這些測試。

完整瀏覽器測試是可選開發工具，固定Playwright1.62.1，不是韌體相依套件：

```powershell
npm install
npx playwright install chromium
npm run test:browser
```

## 編譯

```powershell
.\tools\build.ps1
```

首次自動安裝核心：

```powershell
.\tools\build.ps1 -InstallCore
```

明確指定工具：

```powershell
.\tools\build.ps1 -ArduinoCli 'C:\Tools\arduino-cli.exe' -Python 'C:\Python\python.exe'
```

流程先由 `tools/embed_web.py` 讀取 `web/index.html`、`style.css`、`app.js`，產生不納入Git的 `firmware/RehabEsp32/src/web_assets.h`，再以固定FQBN與自訂分割區編譯。腳本檢查安裝的core版本，將原始碼複製至ASCII暫存目錄編譯，並檢查實際應用程式二進位檔不超過factory分割區4 MiB上限；成功後把產物複製至 `build/artifacts/`，包括Arduino產生的二進位檔及 `toolchain.json`。原專案仍保留在原位置。

Windows的Xtensa linker不能處理部分非ASCII輸出路徑。若系統暫存目錄本身含中文，請指定ASCII路徑：

```powershell
.\tools\build.ps1 -BuildRoot C:\esp32-build\rehab
```

在ASCII專案路徑或Linux環境以一般Arduino CLI手動編譯時，必須先執行嵌入。Windows含中文路徑應使用上述腳本：

```powershell
python tools/embed_web.py
$rehabToolchain = Get-Content toolchain.json -Raw | ConvertFrom-Json
arduino-cli compile --fqbn $rehabToolchain.fqbn --warnings all --build-path build --output-dir build/artifacts firmware/RehabEsp32
```

## 備份原 Flash

**第一次燒錄前先備份原韌體。`flash.ps1` 不會自動執行備份。** 先關閉所有使用COM3的監看程式。以下使用esptool 5系列命令語法；若本機沒有，可在自己的Python環境安裝 `esptool>=5,<6`：

```powershell
python -m pip install 'esptool>=5,<6'
arduino-cli board list
python -m esptool --chip esp32s3 --port COM3 flash-id
New-Item -ItemType Directory -Path backups -Force
python -m esptool --chip esp32s3 --port COM3 --baud 460800 read-flash 0x0 0x1000000 backups/original-16mb.bin
Get-Item backups/original-16mb.bin | Select-Object Length
Get-FileHash backups/original-16mb.bin -Algorithm SHA256
```

16 MB備份應為 **16,777,216 bytes**。保存板型、Flash ID、日期及雜湊，另保留一份安全副本。後續備份使用不同檔名，避免覆蓋首次備份。Flash備份可能包含原系統帳密或其他現場資料，`backups/` 不納入Git。

esptool的讀回／寫入行為以 [官方命令文件](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/basic-commands.html) 為準。

## 燒錄與開機確認

```powershell
.\tools\flash.ps1 -Port COM3
.\tools\monitor.ps1 -Port COM3
```

燒錄使用 `build/artifacts/` 中的產物與固定FQBN；若編譯失敗，不應繼續使用舊產物驗收。監看速度115200，退出監看再進行下一次燒錄。

開機訊息包含：

```text
Rehab ESP32 0.1.1 ready
AP: RehabSetup_<本機 AP MAC>
Portal: http://192.168.4.1/
```

接著確認手機能看到AP、設定頁能載入、尚未連接的機台／讀卡機顯示未就緒，且沒有把未知讀值顯示為0。原生USB已保留給Host，序列監看走CH340而不是原生CDC。

STA連上目標Wi-Fi並取得IP時，序列訊息另會列出區網HTTP入口。也可在AP設定頁的連線概況或路由器DHCP用戶端清單查詢；同區網手機／電腦使用該IP即可存取完整頁面與API。更改SSID／密碼後原STA連結可能失效，請查新IP或改從常開的AP入口進入。

## 分割區與設定

| 名稱 | 起始位置 | 大小 | 用途 |
|---|---|---|---|
| `nvs` | `0x9000` | `0x5000` | 版本化連線設定，範圍到 `0xDFFF` |
| 上傳保留區（不是分割區） | `0xE000` | `0x2000` | Arduino上傳流程的 `boot_app0.bin` 寫入範圍，不可配置NVS或其他分割區 |
| `factory` | `0x10000` | `0x400000` | 4 MiB應用程式 |

Arduino CLI依此核心的上傳配方在 `0xE000` 寫入8 KiB的 `boot_app0.bin`，因此 `0xE000–0xFFFF` 必須保留。NVS配置為20 KiB，完整位於該區之前。core 3.3.11使用內建的PHY初始化資料，此專案不另設 `phy_init` 分割區；升級核心或更換上傳配方時，必須重新核對實際寫入位址，不能只看分割表中有沒有互相重疊。

16 MB Flash並不代表應用程式可用16 MB；**本設定factory app上限為4 MiB（4,194,304 bytes，`0x400000`）**，位置從 `0x10000` 到 `0x410000`（不含結束位址）。Arduino CLI在自訂分割區設定下可能仍印出16 MB的最大程式容量，該行百分比不能作為app可用容量依據；應檢查實際 `partitions.csv` 與產生的分割表，並比較 `RehabEsp32.ino.bin` 大小。

```powershell
$rehabApp = Get-Item build/artifacts/RehabEsp32.ino.bin
if ($rehabApp.Length -gt 0x400000) { throw 'Firmware exceeds the 4 MiB factory partition.' }
$rehabApp | Select-Object Name, Length
```

剩餘Flash未建立運動紀錄檔案系統，未提供OTA。通常重新燒錄app不清空NVS；需要保留現場設定時，不要執行全晶片清除。

本專案刻意不建立core dump分割區，避免將工作記憶體中的運動／卡片資料保存至Flash。啟動時若SDK只印出 `esp_core_dump_flash` 的 `No core dump partition found` 訊息，之後正常印出AP與Portal資訊，這是此配置預期的提示，本身不表示發生panic。若同時有panic、backtrace或連續重啟，仍需依實際錯誤除錯。

## 還原備份

確認使用同一塊板、備份大小與SHA-256吻合，關閉序列監看，再執行：

```powershell
python -m esptool --chip esp32s3 --port COM3 --baud 460800 write-flash 0x0 backups/original-16mb.bin
```

此操作會覆寫目前韌體與備份涵蓋的設定。不要把其他板子的完整Flash直接還原到此板；若裝置啟用Secure Boot／Flash Encryption，需依原系統的保護機制處理，不能用強制選項繞過檢查。

## GitHub CI

`.github/workflows/ci.yml` 執行不依賴硬體的C++／Node測試、Playwright Chromium瀏覽器測試與固定核心編譯。瀏覽器工作以 `npm install --ignore-scripts` 安裝固定套件，再執行 `npx playwright install --with-deps chromium` 及 `npm run test:browser`。CI沒有COM3、不執行燒錄，也無法驗證USB供電、GPIO波形、手機自動彈窗及真實機台讀值。每次硬體驗收結果另寫入 `docs/VALIDATION.md`。
