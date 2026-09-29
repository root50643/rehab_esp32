# 開發與維護

本專案目前未指定開源授權。外部貢獻或發布前，先由專案擁有者確認原程式與提交內容的使用權限。

## 開發環境

依 [BUILD_AND_FLASH.md](docs/BUILD_AND_FLASH.md) 安裝Arduino CLI1.5.1、core3.3.11、Python、Node與桌面C++編譯器。Windows建置腳本使用PowerShell7，並將編譯暫存放在ASCII路徑以避開Xtensa linker的路徑限制。

```powershell
python tools/run_tests.py
.\tools\build.ps1
```

修改前端版面／互動時另執行：

```powershell
npm install
npx playwright install chromium
npm run test:browser
```

## 修改原則

- 在 `web/` 修改頁面，不手動編輯生成的 `web_assets.h`。新資源必須離線可用，不引入CDN或第三方追蹤。
- 保持GPIO／UART控制、TCP、BLE、USB及Portal的責任分離。長時間阻塞呼叫不能放到Web請求處理或GPIO時間流程。
- 通訊變更先修改協定文件與可攜測試；實際APP使用相同解析標頭，避免只測另一份模擬實作。
- 分清楚目標值、已送指令、ACK及機台回報；未證實單位不增加換算。
- 不把未知值填0，不使用其他UART命令刷新0x28／0x3F數值的時效。
- 設定仍須先保存、回覆HTTP，再在停止流程結束後套用；不得因UI刷新寫入NVS。
- 不新增運動資料儲存、雲端傳送、OTA或實體控制按鈕，除非專案需求另行確認。

## 測試與文件

對應新增或修改行為增加有意義的測試：例如錯誤長度、分段收包、逾時、位址類型、卡片去重、409衝突與資料失效，而不是重複實作同一段算法。

前端測試不需要額外套件即可用Node執行；Playwright用於真正瀏覽器版面與互動。硬體改動需要在驗證紀錄中記錄板型、版本、配線及結果。尚未取得設備應標「待驗證」，不能把模擬通過改寫成實機通過。

## 提交內容

只納入此獨立專案需要的程式、測試、人工fixtures與文件。勿納入：

- 原工作目錄的APK、tar／zip、系統備份或其他專案。
- Flash備份、現場帳密、真實SSID／MAC／卡號與個人心率資料。
- 編譯產物、工具安裝、node_modules、日誌、生成的web_assets.h。

`.gitignore`提供基本排除；提交前仍需逐一查看差異與檔案清單。測試日誌與本機資料放 `local/`、`artifacts/`、`backups/`等已忽略目錄。

## PR與Issue

Issue範本請填重現步驟、版本、硬體及去識別資料。PR說明問題、最終行為、測試與尚未驗證項目，通訊／接腳變更應連結更新過的文件。沒有硬體測試時明確說明原因。

提交與推送內容須排除現場帳密、Flash備份、個人讀值及本機測試日誌。發布Release或改變儲存庫可見性，須由擁有者另行決定；一般程式碼推送不代表外設已通過驗收。
