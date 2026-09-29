# 手機設定與 HTTP API

## 連入設定熱點

每台ESP32-S3使用自己的AP MAC組合名稱：`RehabSetup_AA:AA:AA:AA:AA:AA`。冒號保留，英文字母大寫，整體名稱不超過SSID上限。MAC範例不是實際裝置識別碼。

AP為開放網路、最多4個用戶端，固定IP `192.168.4.1/24`。目標Wi-Fi使用STA介面另行連線；手機加入、離開，或STA連線失敗，都不會關閉AP。此版本不提供NAT／網際網路分享。

連線後接受手機的「保持連線／使用此網路」提示。DNS對一般IPv4名稱查詢導向AP，HTTP探測路徑重新導向首頁，並啟用DHCP Captive Portal提示。不同手機系統可能只顯示登入通知而不自動開頁，亦可能受行動網路切換、私人DNS、VPN影響。必要時直接開啟 [http://192.168.4.1/](http://192.168.4.1/)。

AP＋STA共享無線電，連線或掃描時可能有短暫延遲、換頻與手機重連；「AP持續啟用」不代表每一毫秒都能保持傳輸。參考 [Arduino Wi-Fi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html) 與 [Espressif共存說明](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/coexist.html)。

## 從同一區域網路開啟

ESP32連上目標Wi-Fi並取得STA IP後，同區網的手機／電腦可直接開啟 `http://<STA-IP>/`。例如頁面顯示人工測試IP `192.0.2.20`，入口就是 `http://192.0.2.20/`；實際操作請使用現場取得的IP。

AP與STA入口提供相同的完整設定頁、即時讀值及API，沒有登入或額外權限流程。任何可連到上述入口的人都能查看與修改設定。路由器若啟用訪客網路／用戶端隔離，必須讓手機與ESP32具有實際互通的路由，不能只以SSID名稱相同判斷可達。

查詢IP的方式：

1. 在設定熱點的頁面查看「連線概況 → 區域網路入口」，點連結以新分頁開啟。
2. 查看路由器的DHCP租約／已連線裝置清單，找ESP32的STA裝置項目。AP名稱後綴是AP MAC，可能不同於路由器看到的STA MAC。
3. 透過CH340在115200 baud監看序列訊息；STA成功取得IP時會列出區網入口。

STA未連線、尚未取得IP或頁面失去裝置連線時，區網入口顯示 `—`，不保留可誤點的舊IP。連線恢復或IP改變時更新入口，不自動跳頁、不改寫尚未保存的表單。

區網頁面上更改SSID／密碼可能讓ESP32離開目前網路，DHCP亦可能重新分配IP。儲存成功後若頁面中斷，可重新查路由器／序列訊息，或連回持續開啟的設定AP。`192.168.4.1`是AP入口，不是固定的STA區網IP。

Captive DNS及自動入口提示只提供給AP用戶端。AP上的非API未知HTTP路徑會導向首頁；區網上的未知路徑回覆404，不強制轉去AP網址。請使用實際接入介面的IP網址，不支援任意DNS別名、HTTPS或mDNS固定主機名稱。Host不符時，AP的GET探測會導向AP IP，區網請求則回覆403。

## 設定欄位

| 畫面欄位／API名稱 | 格式與行為 |
|---|---|
| 目標Wi-Fi／`wifi_ssid` | UTF-8最多32 bytes；保留頭尾空白。空白字串表示不設定STA網路 |
| Wi-Fi密碼／`wifi_password` | 8–63 bytes密碼或64位十六進位金鑰；明確空字串表示無密碼 |
| TCP主機／`socket_server` | IPv4或ASCII主機名稱，最多253字元，不帶通訊協定、路徑或埠；固定連接9999 |
| 心率MAC／`heart_rate_address` | `AA:BB:CC:DD:EE:FF` 格式，儲存時轉大寫；留白表示停用心率連線 |

預設TCP主機為原程式的 `192.168.0.100`；Wi-Fi與BLE尚未設定。BLE MAC預設留白，避免將某個現場感測器綁定到所有新裝置。

### 掃描與手動輸入

Wi-Fi選單最多保存32個非空、去重SSID，顯示訊號與加密狀態；隱藏SSID直接手填。BLE最多保存64個候選，顯示名稱、MAC、訊號，優先顯示廣播心率服務者，但不排除未廣播該UUID的裝置。裝置沒有名稱會明確顯示「未提供名稱」。

BLE連線使用掃描得到的public／random位址類型，手填MAC也會先透過掃描尋找。此版本不進行帳號配對或BLE bonding，不保證可跟隨會輪換的私人MAC；位址輪換後需重新選取。Wi-Fi與BLE共用掃描協調，另一項忙碌時顯示排隊，不另開一個互相干擾的BLE掃描器。

### 密碼保留

未勾選「更改Wi-Fi密碼」時，POST請求省略密碼欄位，裝置保留既有值。勾選後必須填有效新密碼，或明確選「無密碼網路」；留空輸入框不會被當作清除密碼的默認行為。GET設定只回傳 `has_password`，不回傳實際密碼。

### 保存與套用

先寫入NVS、增加設定版本、回覆HTTP成功，並至少保留500 ms讓回覆送出，再允許套用。運動追蹤、啟停脈衝、啟停回覆、控制佇列、阻力指令／ACK或UART回覆尚未完成時保留待套用版本，同時等待TCP必要回覆、寫入與收送佇列完成。多次儲存只保留最新設定。

畫面「設定已保存」不等於Wi-Fi／TCP已連線成功。`settings`為最新保存值，`active`為目前生效值，`pending`顯示兩者是否等待切換。重啟直接採用最新保存設定，不恢復運動狀態。即使密碼錯誤，設定AP仍能操作。

## API

同源HTTP，服務監聽AP與STA，並確認請求的本機目的IP為設定AP或目前已連線的STA IP。兩個入口均可使用下列API。POST設定必須使用 `Content-Type: application/json`，body上限2048 bytes，僅接受列出的平面欄位，不接受未知／重複欄位、陣列、巢狀物件或Unicode空字元。`revision`是1至4294967295的整數；跨來源寫入仍被拒絕。前端使用相對API路徑，從哪個入口開啟就向該入口請求，不硬編碼AP位址。

| 方法 | 路徑 | 用途 |
|---|---|---|
| GET | `/api/status` | 目前裝置狀態及最後封包快照 |
| GET | `/api/config` | 設定版本、保存值、生效值及保存狀態 |
| POST | `/api/config` | 驗證版本並保存設定 |
| POST | `/api/scan/wifi` | 排入Wi-Fi掃描 |
| GET | `/api/scan/wifi` | 查詢Wi-Fi掃描進度與結果 |
| POST | `/api/scan/ble` | 排入BLE掃描 |
| GET | `/api/scan/ble` | 查詢BLE掃描進度與結果 |

GET設定範例（人工資料）：

```json
{
  "revision": 3,
  "pending": false,
  "storage_ok": true,
  "settings": {
    "wifi_ssid": "Lab-Test",
    "has_password": true,
    "socket_server": "192.168.0.100",
    "heart_rate_address": ""
  },
  "active": {
    "wifi_ssid": "Lab-Test",
    "socket_server": "192.168.0.100",
    "heart_rate_address": ""
  }
}
```

只更新主機且保留密碼的POST範例：

```json
{
  "revision": 3,
  "wifi_ssid": "Lab-Test",
  "socket_server": "rehab-server.local",
  "heart_rate_address": ""
}
```

成功回覆包含 `ok: true`、最新 `revision`、`pending`與設定快照。主要錯誤碼：400格式／欄位不正確、403非允許的本機介面／跨來源、404未知路徑、409設定版本衝突、503NVS保存失敗。409不自動覆蓋最新設定，前端保留使用者輸入，由使用者重新載入確認。

掃描GET回覆形狀：

```json
{"state":"done","results":[],"error":""}
```

`state`為 `idle`、`queued`、`scanning`、`done` 或 `error`。Wi-Fi項目包含 `ssid/rssi/secure/channel`；BLE項目包含 `name/address/address_type/rssi/heart_rate`。`heart_rate`代表廣播中看見心率服務，不是實際已訂閱通知的證明。

狀態API的分組與完整欄位說明見 [DEBUGGING.md](DEBUGGING.md)。未知數值使用JSON `null`，最近錯誤／封包可能是空字串。API資料只供觀察，不會因讀取狀態而啟停機台或增加控制命令。
