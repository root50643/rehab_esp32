# 通訊協定與移植相容性

## 來源

本地行為基準：`20260701_C521M/pi/HeartBeatRate/multi_thread.py.20260910`。

```text
SHA-256: 0A5DF11567F87C02860843466E8882966290D01DF787D2BF62CCF871859CF0DA
```

這份文件與C++測試只使用協定及人工測試資料，不複製原工作目錄的帳密、APK、系統映像或運動資料。機台欄位意義依原程式解析行為，未經廠商工程規格確認的換算比例不推測。

## 共用封包

```text
HEAD | COMMAND (1 byte) | LENGTH (1 byte) | DATA (LENGTH bytes) | CHECKSUM | TAIL
```

| 通道 | HEAD | TAIL |
|---|---|---|
| TCP | `7A` | `7F` |
| 機台UART | `55` | `90` |

```text
CHECKSUM = (0xFF - ((COMMAND + LENGTH + sum(DATA)) & 0xFF)) & 0xFF
```

長度不包含HEAD／COMMAND／LENGTH／CHECKSUM／TAIL。數值欄位的2-byte整數為big-endian，BLE與CCID另依自己的協定使用little-endian。資料中的HEAD／TAIL值不是分隔點，解析器按照LENGTH收滿後再驗證。支援拆包、連續黏包、雜訊與錯誤checksum恢復；不完整框架2秒逾時。

## TCP client

預設主機 `192.168.0.100`，固定9999埠。ESP32主動連線，未連上時重試；沒有在ESP32上開另一個機台控制TCP server。

一般部署透過目標Wi-Fi的STA路由連到伺服器；除錯時，也可將伺服器設為連在設定AP上的電腦IP，例如該電腦取得的 `192.168.4.x`，由ESP32主動連入其9999埠，無需NAT。這個TCP client路由與設定HTTP服務是不同功能；HTTP頁面另可從AP及同區網的STA IP開啟。路由或本機IP變更時重建TCP連線。

| 命令 | 方向／資料 | 行為 |
|---|---|---|
| `20` | 主機→裝置，1-byte selector | `00`讀卡機、`01`機台Start狀態、`02`BLE通知訂閱 |
| `20` | 裝置→主機，`selector C8/C9` | 可用／不可用；BLE以訂閱狀態回覆，與前端三筆通知就緒狀態不同 |
| `21` | 裝置→主機，UID bytes | 按原順序送出完整UID，不以整數轉換抹除前導零 |
| `21` | 主機→裝置，`C8`或`C9` | 明確確認／拒絕UID，診斷頁據此顯示 |
| `22` | 主機→裝置，`00`／`01`／`02` | 停止／啟動／暫停 |
| `22` | 裝置→主機，`selector C8/C9` | Start／Stop脈衝完成後回覆；暫停僅回覆，沿用原行為 |
| `23` | 裝置→主機，`selector value` | `00`RPM、`01`阻力、`02`BLE心率；只在追蹤期間且值變更時送出，不等待ACK |
| `24` | 裝置→主機，`00` | 原異常停止通知流程 |
| `25` | 主機→裝置，1或2-byte阻力 | >0時發出機台0x44；0沿用不實際下送的行為 |
| `25` | 裝置→主機，`C8` | 收到阻力命令，不代表機台已到達目標 |

0x23是單位元組值；RPM、阻力或心率超過255時不截斷傳送，仍保留於網頁讀值。BLE0 BPM不作有效心率回報。機台0x28／0x3F需為5秒內的數值，BLE需為10秒內的有效值才送出。待送數值每類只保存最新一筆，寫入socket前再驗證時效及來源，不能將已失效心率或停止後的舊讀值延後送出。

需等待回覆的傳送使用500 ms間隔，最多送3次；收到有效封包即結束該輪等待以相容原程式，但只有等待UID回覆時收到明確0x21回覆才顯示「主機確認UID」。TCP控制命令、優先回覆及最新0x23數值即使在等待事件回覆時仍能被處理，避免狀態查詢被阻塞。

每次成功連線增加generation，清除舊連線佇列，不在重新連線後重放離線刷卡事件或前一個連線的控制命令。已開始的GPIO脈衝仍按時完成。既有協定沒有交易ID，因此同一連線內的重試不提供exactly-once保證。

人工查詢範例：

```text
查詢讀卡機：7A 20 01 00 DE 7F
回覆可用：  7A 20 02 00 C8 15 7F
```

## 機台UART

57600、8N1。以下offset皆從DATA起算：

| 命令 | LENGTH | DATA內容／回覆 |
|---|---|---|
| `27` | 1 | `00`Idle、`01`Start、`02`Pause、`03`End；原樣回覆 |
| `28` | 12 | 分[0..1]、秒[2]、距離[3..4]、熱量[5..6]、機台BPM[7]、RPM[8..9]、功率[10..11] |
| `30` | 0 | 機台啟動事件；回覆 `30 01 01`的資料型態（COMMAND、LENGTH、DATA），完整封包另加checksum／邊界 |
| `3F` | 6 | 阻力[0..1]、功率[2..3]、扭力[4..5] |
| `44` | 4（送出） | 目標阻力2 bytes、功率欄位 `00 00` |
| `44` | 1（收到） | 阻力命令ACK，資料需等於對應送出命令的checksum |

0x28／0x3F的ACK資料是原封包checksum，LENGTH=1。0x44的ACK不再被ACK一次，避免回覆迴圈。阻力ACK等待3秒，逾時顯示錯誤；已送出指令值與後續機台回報值分開保存。等待前一筆阻力ACK期間，新的目標合併為最新待送值；0目標不下送，並取消尚未寫出的阻力目標。機台回覆先進有界FIFO，UART緩衝足夠時才寫出整個封包。

Start為GPIO4 HIGH 500 ms；Stop為GPIO5 HIGH 3000 ms，LOW後等待500 ms。脈衝以時間狀態機執行，期間仍接收UART/TCP。UART長時間空讀維持原程式的異常停止處理；實際觸發時序與機台反應須完成實機驗收。

除錯API的 `machine.state` 僅由收到的0x27／0x30更新，尚未收到時為null，使用自己的 `state_age_ms` 判斷時效。相容原控制流程所需的本機判斷另放 `machine.control_state`；輸出脈衝不能充當機台已回報狀態的證據。

## BLE心率

- 標準Heart Rate Service `0x180D`，訂閱Measurement `0x2A37` 的CCCD `0x2902`。
- Flags bit0決定8／16位元BPM；16位元為little-endian。依Flags驗證Energy Expended與RR-Interval欄位的完整性。
- 連續3筆有效且非零通知後就緒。通知10秒逾時，清除舊心率；失敗後3秒重試。連線與GATT操作均有期限。
- Battery Service `0x180F`／`0x2A19`為選用，讀取失敗不阻止心率。
- 單一有上限的掃描器保存候選，保留public／random位址類型，避免只用MAC字串假定所有裝置均為public。

標準定義見 [Bluetooth Heart Rate Service](https://www.bluetooth.com/specifications/specs/heart-rate-service-1-0/)；連線實作使用core內附NimBLE，並非Raspberry Pi的bluepy。

## CL-2000 CCID／APDU

直接使用ESP32-S3 USB Host，依實際描述元取得CCID介面與bulk／interrupt端點。僅接受VID `1206`／PID `2107`、slot0、automatic APDU能力及支援的訊息長度，不把任意USB鍵盤式讀卡機當成CL-2000。

| 指令 | 值／內容 |
|---|---|
| `PC_to_RDR_GetSlotStatus` | `65` |
| `PC_to_RDR_IccPowerOn`／PowerOff | `62`／`63` |
| `PC_to_RDR_XfrBlock` | `6F` |
| `RDR_to_PC_DataBlock`／SlotStatus | `80`／`81` |
| 讀UID APDU | `FF CA 00 00 00` |
| 蜂鳴 APDU | `FF E1 02 01 0C A0 FC 5C A0 FD 0A A0 FE 0A A0 FF 03` |

CCID標頭10 bytes，資料長度使用little-endian。驗證訊息類型、slot、sequence、長度、狀態與chain欄位；UID必須有成功狀態字 `90 00`，完整轉成HEX並保留前導零。使用有期限的USB傳輸，裝置移除／逾時時終止並恢復。

同卡持續在場只產生一次UID事件；PowerOff造成的inactive-present不視為移除，必須觀察到真正移除才重新允許讀卡。蜂鳴指令沿用來源程式，是否支援及實際聲音需CL-2000實機確認。

參考 [libccid收錄的CL-2000描述元](https://raw.githubusercontent.com/LudovicRousseau/CCID/master/readers/SYNNIX_CL-2000.txt) 及 [USB-IF CCID規格頁](https://www.usb.org/document-library/device-class-specification-usb-chip-smart-card-interface-devices)。公開描述元用來建立能力檢查與人工測試，不代替目前連接讀卡機的實機驗證。

## 與原版本的明確差異

| 項目 | ESP32版本 |
|---|---|
| 設定來源 | 不再編輯Python常數；手機頁面＋NVS，版本衝突保護，運動中延後套用 |
| BLE預設位址 | 留白，首次安裝由使用者選取感測器 |
| 封包處理 | 長度與checksum驗證，支援拆黏包、逾時及恢復 |
| IO執行 | 非阻塞脈衝、獨立TCP／BLE／USB工作，Portal持續可用 |
| 有效資料 | 區分未收到、失效及最後讀值；BLE與機台心率分開 |
| USB讀卡 | 不依賴Linux PC/SC，使用受限CCID APDU驅動；實機尚需核對 |
| 儲存 | 不保存運動歷史；Flash只寫連線設定與平台所需資料 |
| 稽核 | 有桌面測試與來源雜湊，實體驗收另行記錄 |
