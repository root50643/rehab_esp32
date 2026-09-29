# 變更紀錄

## 0.1.1 — 2026-09-29

- ESP32連上目標Wi-Fi後，同區網可由實際STA IP開啟完整設定頁與API，AP持續啟用。
- 連線概況顯示可點擊的AP／區網入口；斷線或沒有IP時清除區網連結，更新不覆蓋未儲存表單。
- Captive DNS與AP探測導向只作用於AP，區網未知路徑回覆404。HTTP使用實際介面IP及Origin檢查，不支援任意DNS別名。
- 補充STA IP查詢、切換Wi-Fi後恢復連線的方法，並新增入口狀態與IP變更的前端／瀏覽器測試。

## 0.1.0 — 2026-09-29

首次ESP32-S3移植，依據 `multi_thread.py.20260910` 的控制與通訊行為。

### 新增

- 持續開啟且帶AP MAC後綴的設定Wi-Fi、Captive Portal與繁體中文RWD頁面。
- Wi-Fi／BLE掃描與手填、TCP主機設定、NVS保存、revision衝突處理及運動中延後套用。
- BLE標準心率解析、可選電池、有限掃描、訂閱／逾時／重連狀態。
- TCP／UART封包解析、拆黏包恢復、非阻塞GPIO脈衝及連線世代處理。
- CL-2000 USB Host CCID／APDU驅動、UID前導零保存、卡片在場去重與插拔處理。
- 即時值、各來源時效、狀態查詢、最後封包與診斷計數；不儲存運動紀錄。
- 可攜C++、Node及可選瀏覽器測試，固定工具鏈、GitHub CI與詳細接線／操作文件。

### 驗證狀態

本版本的編譯、燒錄及各項硬體驗收結果，以 [docs/VALIDATION.md](docs/VALIDATION.md) 為準。版本號不表示所有外設已完成實測，也不表示已發布GitHub Release。
