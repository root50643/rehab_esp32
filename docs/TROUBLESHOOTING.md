# 故障排除

先確認正在使用的韌體版本、`toolchain.json` 與 [驗證紀錄](VALIDATION.md)。手機頁面中的狀態及最後封包通常比反覆重啟更容易定位問題。

| 症狀 | 檢查方式／處理 |
|---|---|
| 看不到COM3 | 以 `arduino-cli board list`／Windows裝置管理員找實際埠；確認資料線及CH340介面，不把原生USB Host當作燒錄口 |
| 燒錄說port busy | 關閉monitor、Arduino IDE序列監看與其他使用該埠的程式，再重試 |
| 等待連線到晶片 | 檢查USB線、供電與開發板下載模式；依該板BOOT／RESET操作，不能套用未知板子的按鍵位置 |
| Windows編譯在link失敗 | 使用 `build.ps1` 的ASCII暫存路徑；必要時加 `-BuildRoot C:\esp32-build\rehab`，不要搬動原專案來回避中文路徑 |
| 找不到web_assets.h | 使用 `build.ps1` 或先執行 `python tools/embed_web.py`；生成檔不納入Git |
| BLE API／NimBLE編譯錯誤 | 核對core為3.3.11及FQBN為ESP32-S3，移除意外覆蓋SDK的同名第三方BLE庫 |
| 開機持續重置 | 先只接CH340與控制板，檢查供電、PSRAM設定及序列啟動錯誤；分步加入外設 |
| 開機出現 `No core dump partition found` | 本專案刻意不配置core dump，避免將RAM中的運動／卡片資料保存到Flash。只有此訊息且後續正常開啟AP時，屬預期提示，本身不是panic；若另有panic／backtrace或連續重啟，須繼續排查 |
| 找不到設定AP | 先看115200開機訊息是否印出AP；核對實際MAC後綴；其他裝置可能有相似前綴 |
| 已連Wi-Fi但沒彈設定頁 | 選保持連線，再開 `http://192.168.4.1/`；檢查手機是否切回行動網路、VPN或其他Wi-Fi |
| 頁面403 | 使用裝置實際AP IP或目前STA IP網址，不使用DNS別名／反向代理主機名；Host不符或跨來源POST仍會被拒絕 |
| 同區網打不開頁面 | 從AP頁、路由器DHCP清單或115200序列訊息確認目前STA IP；使用http://，確認手機與ESP32可互通，未被訪客網路／用戶端隔離 |
| 區網修改Wi-Fi後頁面中斷 | 裝置可能已改連另一個SSID或取得新IP；查路由器／序列訊息，或回到常開的設定AP，確認保存結果 |
| 區網未知路徑顯示404 | 這是預期行為；開啟STA IP根目錄 `/`。只有AP上的Captive Portal路徑會導回AP首頁 |
| 目標Wi-Fi搜尋不到 | 確認2.4 GHz；隱藏SSID手動輸入；讓另一項掃描完成後重試 |
| Wi-Fi密碼一直錯 | 更換網路時勾選「更改密碼」；不勾會保留舊密碼。無密碼網路需明確選擇，不能只留空 |
| 儲存時HTTP409 | 另一頁已改過設定。記下需要的輸入，按重新載入確認最新版本後再儲存 |
| 設定已保存但沒切換 | 查看tracking、脈衝、queued_controls、control_ack_queued、queued_uart_replies、level_queued／level_ack_pending，再看TCP writing／awaiting_ack／佇列；另保留500 ms讓HTTP回覆送出 |
| NVS保存失敗／503 | 查看 `storage_ok`與錯誤。不要把「已改表單」當成保存成功；備份後再檢查Flash／分割區 |
| Wi-Fi已連上但TCP未連 | 確認主機IP／DNS、固定9999埠、主機監聽位址、防火牆與路由器用戶端隔離 |
| 沒有可用路由器但要測TCP | 電腦連入設定AP並在9999埠監聽，將socket_server設為電腦取得的192.168.4.x位址；核對電腦防火牆，不要填ESP32自己的192.168.4.1 |
| BLE選單沒有裝置名稱 | 裝置廣播可能沒有名稱；用MAC辨識。確保裝置正在廣播，沒有被另一台手機獨占連線 |
| BLE連線但無BPM | 分別看connected／subscribed／notifications；確認提供180D/2A37而非只有專有服務；電池欄位空白不阻止心率 |
| 換電池後BLE MAC變了 | 重新掃描選取；使用隨機私有位址的裝置可能輪換，本版本不以bonding追蹤身份 |
| BPM是— | 尚未有效通知、通知格式失敗、0 BPM或逾時已清除；查看last_error與通知HEX |
| 心率>255但主機没收到 | TCP0x23只有1 byte，本版本保留原協定，不截斷高值；本機頁面仍顯示完整16-bit值 |
| UART沒有回報 | 核對TX/RX交叉、57600/8N1、電位與GND；先不要將未知機台訊號直接接GPIO |
| UART checksum錯誤增加 | 比對原始HEX、接線、電氣準位與鮑率；封包資料中出現55或90不一定是錯誤 |
| RPM過期但UART在線 | 查看age28_ms；狀態心跳或0x3F仍可在線，不代表0x28數值正在更新 |
| 阻力要求與實際不同 | 已收到TCP命令、等待下送、已送UART、收到ACK與收到0x3F是不同階段；檢查level_queued／level_ack_pending與3秒逾時，新目標會合併等待 |
| Pause沒使機台停下 | 原程式的Pause命令僅回覆，本版本維持此行為；需要改變時必須確認機台協定與控制需求 |
| CL-2000一直等待 | 核對USB Host D±、保護5V供電、讀卡機是否接ESP32而非電腦；不可插在CH340資料路徑 |
| unsupported_reader | 查看VID／PID、CCID描述元／能力；此驅動不是通用HID鍵盤讀卡器或所有TPDU讀卡器的實作 |
| 卡片在場但無UID | 看ATR、CCID RX、last_sw；USB傳輸成功不等於APDU9000成功 |
| 同卡放著不重報 | 這是預期去重行為；取走卡片再感應。斷線期間的刷卡不會連線後重放 |
| 刷卡顯示逾時未確認 | 不代表封包沒到主機；查看主機0x21 ACK與網路。重試可能產生重複封包 |
| 手機頁讀值停止 | 確認在前景且仍可達AP或目前STA入口；中斷會標示過期，若STA IP已變更需重新開啟新網址 |

## 回報問題時提供

- 韌體版本／commit、core與CLI版本、作業系統、板型及供電方式。
- 問題步驟、預期與實際結果，重現頻率，是否僅在同時Wi-Fi／BLE掃描時出現。
- 去識別的相關狀態、錯誤計數與最小必要封包。
- 對硬體問題提供接線圖與電位量測；不要用照片中的排針位置代替GPIO名稱。

請勿提交Flash備份、真實帳密、完整個人讀值、未遮蔽的UID或現場SSID。能用 `tests/fixtures` 的人工資料重現時，優先提供可重現測試。
