# 驗證紀錄

此文件記錄本次ESP32移植的實際證據。不得以原Raspberry Pi測試、桌面模擬或CI成功替代尚未執行的實機驗收。

## 本次環境

| 項目 | 資訊 |
|---|---|
| 日期 | 2026-09-29，Asia/Taipei |
| 韌體版本 | 0.1.1；同日0.1.0的外設待驗項目仍保留 |
| 工作平台 | Windows／PowerShell，專案位於含中文路徑的工作目錄 |
| 已連接控制板 | ESP32-S3、16 MB Flash、8 MB PSRAM，CH340／COM3 |
| 工具鏈 | Arduino CLI 1.5.1，ESP32 core 3.3.11，完整設定見toolchain.json |
| 測試資料 | 人工封包與 `tests/fixtures/status.json`，非真實個人心率／卡號 |

## 已完成與待完成

| 項目 | 狀態 | 證據／限制 |
|---|---|---|
| Node前端測試 | 通過 | 18項；另涵蓋AP／LAN入口、無IP／斷線、IP變更與未儲存輸入保留 |
| C++協定／狀態／BLE／CCID／設定與DNS測試 | 通過 | 5個測試執行檔，g++ C++17、-Wall -Wextra -Werror、AddressSanitizer及UndefinedBehaviorSanitizer；不代表外設實機通過 |
| 分割表安全回歸測試 | 通過 | Python unittest 8項；拒絕NVS與Arduino固定燒錄區重疊、驗證實際binary表及MD5 |
| 真實瀏覽器RWD | 通過 | 360／390／768／1440px；人工API完整測試及真實ESP32區網頁面皆通過，實機確認設定載入、即時輪詢、入口及未儲存輸入保留 |
| 畫面截圖 | 已產生 | `docs/images/dashboard-mobile.png`、`diagnostics-desktop.png`；皆為實際UI＋人工資料 |
| 固定核心完整編譯 | 通過 | core 3.3.11；使用ASCII暫存路徑完整建置，app 1,342,080 bytes／4 MiB；本輪編譯無警告 |
| 首次16MB Flash備份 | 通過 | 燒錄前完整讀回16,777,216 bytes；檔案留在忽略目錄，SHA-256見下方 |
| COM3燒錄與重開機 | 通過 | esptool 5.3.1寫入並核對hash；開機確認Flash 16,777,216 bytes、PSRAM 8,388,608 bytes，未觀察到panic／重啟迴圈 |
| AP名稱與無密碼廣播 | 通過 | SSID後綴與Windows掃描取得的實際AP BSSID一致；2.4GHz／Open；Windows WLAN兩次記錄成功association |
| STA連線與完整區網設定頁 | 通過 | 0.1.1透過目標Wi-Fi的DHCP IP完成首頁／CSS／JS、status/config API及真實瀏覽器驗證；重啟自動恢復區網入口 |
| AP側Captive Portal與STA失敗情境 | 部分驗證；待完成 | 先前Windows AP切換限制仍未排除；AP側HTTP／DNS、STA失敗與手機重連尚待獨立實測 |
| NVS保存與重開機保留 | 通過 | 區網POST相同連線設定且省略密碼，revision由3增至4並套用；重啟後revision仍4且可重新連入原Wi-Fi，未更改原設定 |
| 運動中延後套用 | 模擬覆蓋；實機待驗證 | 此次區網測試未發送啟停／阻力命令，STOP後套用仍需實機驗收 |
| 區網HTTP Host／Origin及版本衝突 | 通過 | 偽造Host/Origin與跨來源POST回403；錯誤revision回409且原設定不變；預設:80寫法可用；未知區網路徑回404且不導向AP |
| TCP模擬伺服器互通 | 實機待驗證 | 拆黏包等桌面測試通過；PC未能持續留在AP，實機TCP階段尚未執行 |
| BLE心率感測器 | 待接實體設備驗證 | 通知、三筆就緒、斷線／10秒逾時、random address、長時間共存 |
| CL-2000 USB Host | 待接實體設備驗證 | 供電、描述元、ATR、UID、蜂鳴、同卡去重、移除與重插 |
| 復健機UART／GPIO | 待接實體設備驗證 | 必須先確認電氣介面與安全接線，再驗證機台實際反應 |
| 手機自動Captive Portal | 待iOS／Android實機驗證 | 瀏覽器測試不能證明手機系統自動彈窗 |
| GitHub CI | 已設定；以Actions執行紀錄為準 | 本機等效測試與固定核心編譯已通過；遠端結果見[GitHub Actions](https://github.com/root50643/rehab_esp32/actions)，不以CI代表硬體驗收 |

## 本機實測限制

目前僅接上ESP32-S3的CH340 USB介面，尚未連接復健機、CL-2000或心率感測器。沒有量測實際GPIO波形，也沒有以機台ACK或讀值證明阻力已套用。

Windows WLAN事件於2026-09-29 13:51:35及13:53:37記錄成功連上設定AP（事件8001），下一秒即記錄中斷（事件8003、ReasonCode 2）。後續netsh連線另回傳錯誤87。未找到仍執行的測試還原程序，也未確認中斷來源，不能據此判斷是韌體或Windows策略問題。原Wi-Fi設定已恢復，臨時測試profile已移除。測試帳號也沒有建立臨時Windows防火牆規則的權限；未停用防火牆或修改系統策略。

上述是0.1.0階段的AP連線限制。0.1.1新增區網入口後，已從原區網完成真實網頁、設定API及NVS重啟保存驗證；它不代替AP側Captive Portal、Wi-Fi／BLE實機掃描、運動中延後套用及TCP控制整合。可在能持續連入AP的電腦執行 `tools/ap_smoke.py`；完整控制測試需要明確加上 `--unwired-machine`，且機台控制線必須實際拔除。手機AP手動入口仍為 `http://192.168.4.1/`。

區網重現測試（將IP換成實際STA IP）：

```powershell
python tools/lan_smoke.py 192.168.0.154
python tools/lan_smoke.py 192.168.0.154 --save-unchanged
# 重啟後，以前一步列出的revision驗證保存：
python tools/lan_smoke.py 192.168.0.154 --expect-revision 4
```

`--save-unchanged`只在控制器閒置時保存相同設定，省略密碼欄位，只增加設定版本；不發送GPIO／TCP控制命令。不公開現場SSID、心率、卡號或Flash備份。DHCP位址可能變更，可由115200序列輸出的 `LAN portal: http://<IP>/` 或路由器DHCP清單查詢。

## 本次燒錄產物

- app：`build/artifacts/RehabEsp32.ino.bin`，1,342,080 bytes，SHA-256 `825E67C0083C0B1A1E583B9A8BE3AC0346CFE15744E71E199B8EEB9660C930B5`。
- 分割表：`build/artifacts/RehabEsp32.ino.partitions.bin`，SHA-256 `EFAE30C07E09C598C9ADA47D504CC6758D760FD947178F036757B42995355ECB`。
- NVS：`0x9000`、長度`0x5000`；`0xE000–0xFFFF`保留給Arduino uploader；factory app從`0x10000`開始、容量4 MiB。
- 核心在開機時可能印出 `No core dump partition found`；本專案刻意不設core dump分割區，詳見[故障排除](TROUBLESHOOTING.md)。

## 重現自動測試

```powershell
python tools/run_tests.py
node --test tests/frontend.test.cjs
```

可選瀏覽器測試：

```powershell
npm install
npx playwright install chromium
npm run test:browser
```

Playwright固定於 `package.json`，僅供桌面UI測試；ESP32執行不需要Node.js或npm。測試程式在 `tools/browser_test.cjs`，以本機HTTP與人工API運作。修改UI後應重新產生／核對截圖。

## 實機驗收步驟

| 情境 | 操作 | 通過條件 |
|---|---|---|
| AP首次設定 | 清楚辨識測試裝置AP，手機連入 | 後綴與本機AP MAC一致，手動頁面可開啟 |
| STA失敗 | 在測試網路輸入錯誤密碼 | 錯誤可見，設定AP持續存在，可改回正確值 |
| 設定持久化 | 保存人工設定後重啟 | 設定保留、運動狀態未恢復、RAM計數重置 |
| 多頁設定衝突 | 兩個頁面載入同版本，再各自保存 | 後儲存者收到409，輸入保留，不覆蓋新設定 |
| 運動中修改 | 受控啟動後保存新連線值 | 顯示pending，停止脈衝與回覆完成後才重新連線 |
| BLE斷線 | 移開／關閉測試感測器 | 10秒有效通知逾時後舊值失效，恢復時可重連 |
| 機台獨立時效 | 停止0x28、繼續0x27／0x3F | RPM等0x28值過期，0x3F值保持獨立判斷 |
| 阻力 | 發送人工目標值並觀察UART | 要求／已送出／機台回報分開，ACK可對應 |
| GPIO | 示波器／邏輯分析儀量測 | Start500 ms、Stop3000 ms＋500 ms等待符合規格 |
| 讀卡去重 | 卡片持續放置、拿開再感應 | 停留不重複發事件，移除再放置可重新回報 |
| USB重插 | 拔除讀卡機後接回 | 頁面狀態正確、無卡號事件重放、服務可恢復 |
| TCP恢復 | 斷開伺服器再開啟 | 可重連，不重放離線刷卡與舊控制命令 |
| 長時間共存 | 網頁刷新＋BLE＋UART＋USB運行 | 無持續Heap下降／重啟，逾時與錯誤可解釋 |

硬體測試記錄應含日期、韌體雜湊、板型、接線／電源、外設型號、步驟、預期、實際結果。只公布已去識別的結果；原始Flash、現場密碼、卡片UID與個人讀值留在忽略目錄。

## 原Flash備份證據

首次燒錄前於2026-09-29由COM3讀回完整16 MB Flash，檔案為本機忽略目錄中的 `backups/original-20260929-132109.bin`（16,777,216 bytes）。SHA-256：

```text
22264EAEB786C43A5F68332AAA6C3B7C939FA64EFB56C9E021E87ECE42C58170
```

Flash備份含裝置原始內容，僅供本機還原，不納入Git追蹤或公開附件。
