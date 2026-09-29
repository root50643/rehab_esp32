# 即時除錯與資料判讀

首頁顯示BLE心率、機台RPM、機台實際回報阻力；下方「機台回報數值」列出其他讀值。「進階通訊資訊」展開後可看協定、USB與各服務的狀態。

連線概況提供「設定熱點入口」與「區域網路入口」。STA連上目標Wi-Fi後，後者顯示可從同區網開啟的HTTP連結；斷線或沒有IP時顯示 `—`。入口更新不修改設定表單，連結以新分頁開啟。

## 更新與失效

- 前端約每1秒GET `/api/status`，前一請求結束才排下一次；網路慢時不堆積請求。背景頁停止輪詢，返回時重新取得資料。
- `—`代表尚未取得；0是實際值，不將未知值補0。
- BLE有效通知10秒逾時會清除有效讀值並重新連線。三筆連續有效通知後 `ready=true`。
- 機台0x28、0x3F使用各自的更新時間；5秒未更新標示過期。0x27狀態封包持續到達不會讓舊RPM、距離或阻力恢復有效。
- 5秒是讀值新鮮度門檻，不是自動按下Stop的時間。原控制器對UART長時間空讀的處理是另一條約32次空讀流程。
- 手機與ESP32連線中斷時，畫面保留最後資料並標示過期，持續嘗試重連；所有數值留在RAM，沒有歷史曲線或紀錄檔。

`age_ms`是裝置計算的距今毫秒，非UTC時間戳；開機時間也不是日曆時間。感測值被多個工作分別更新，整份HTTP回覆是各服務有鎖保護的連續快照，不承諾全系統在同一微秒取樣。

## 欄位對照

### 裝置、AP及Wi-Fi

| JSON欄位 | 意義 |
|---|---|
| `firmware`、`uptime_ms` | 韌體版本與本次開機運行時間 |
| `pending` | 已保存的新設定尚待套用 |
| `ap.ssid/ip/clients` | 設定熱點名稱、IP、連入用戶端數 |
| `wifi.state/ssid/ip/rssi` | STA狀態、目前目標SSID、取得的IP與訊號 |
| `wifi.last_error` | 最近Wi-Fi連線錯誤；斷線原因碼用於進一步定位 |
| `health.free_heap/min_free_heap` | 目前／本次開機最低可用Heap，單位bytes |

### BLE心率

| JSON欄位 | 意義 |
|---|---|
| `ble.state/connected/subscribed/ready` | 工作階段、GATT連線、2A37通知訂閱、三筆有效通知就緒 |
| `ble.bpm` | BLE心率，支援8／16位元；超過255仍可在頁面查看，不截斷送入舊TCP欄位 |
| `ble.battery` | 選用2A19電量百分比；沒有服務或讀取失敗不阻止心率工作 |
| `ble.name/address/address_type/rssi` | 名稱、目標MAC、掃描得到的位址類型及訊號；RSSI不是持續量測的即時射頻品質 |
| `ble.notifications/consecutive/age_ms` | 累計有效通知數、目前連續有效通知數、最後有效通知距今 |
| `ble.disconnects/last_error` | BLE斷線計數與最近錯誤 |
| `ble.last_notification_hex` | 最後BLE通知前64 bytes，較長以省略標記顯示 |

BLE心率與機台心率來自不同來源，不互相覆蓋。收到0 BPM不延長最後有效通知期限。非完整通知會被解析器拒絕，而不是讀取越界資料。

### 機台數值與GPIO

| JSON欄位 | 來源／意義 |
|---|---|
| `machine.state/state_age_ms` | 僅由UART 0x27／0x30更新的機台回報狀態與距今時間，未收到為null；5秒未更新標示過期 |
| `machine.control_state/tracking` | 本機控制判斷與控制器是否追蹤數值；不是機台已回報的狀態，不用來補未知的state |
| `machine.uart_online/age_ms` | 近期是否有有效UART封包、最後有效封包距今 |
| `machine.age28_ms/age3f_ms` | 兩組數值各自的更新時間，用於新鮮度判斷 |
| `machine.rpm/machine_bpm` | 0x28轉速／機台心率 |
| `machine.minutes/seconds` | 0x28運動時間；保持原始分／秒欄位 |
| `machine.distance_raw/calories_raw/watt28_raw` | 0x28距離、熱量、功率原始值 |
| `machine.reported_level/watt3f_raw/torque_raw` | 0x3F實際阻力、另一組功率、扭力原始值 |
| `machine.target_level` | 主機0x25要求的阻力 |
| `machine.sent_level` | 已寫入機台UART的阻力命令值 |
| `machine.level_ack_pending` | 正在等待對應阻力命令的UART ACK |
| `machine.level_queued` | 最新阻力目標尚待寫入UART；上一筆仍在等ACK時會合併較新的目標 |
| `machine.queued_controls` | 等待執行的Start／Stop命令數，不包含目前已在輸出的脈衝 |
| `machine.control_ack_queued` | 已完成啟停脈衝，但其回覆尚待交給TCP優先佇列 |
| `machine.queued_uart_replies` | 待寫入UART的機台回覆數，與主機控制命令佇列不同 |
| `machine.pulse` | idle／start／stop／stop_settling脈衝階段 |
| `machine.rx_hex/tx_hex` | 最後有效機台封包及最後寫出的機台封包 |
| `machine.checksum_errors/framing_errors/timeouts/last_error` | 校驗、格式、逾時計數與最近錯誤 |

距離、熱量、功率、扭力的工程單位與比例尚未經機台規格確認，頁面不自行標成km、kcal、W或Nm。請先建立實機基準再加入換算，並同步修改協定文件與測試。

### TCP

| JSON欄位 | 意義 |
|---|---|
| `tcp.connected/server/port/generation` | 連線、目的主機、9999埠、目前連線世代 |
| `tcp.reconnects/last_error` | 成功建立連線計數（含第一次）與最近錯誤 |
| `tcp.writing` | TCP工作已保留並正在傳送一個封包；布林旗標不是資料量 |
| `tcp.awaiting_ack` | 正在等待一般事件的回覆，例如UID；不表示0x23數值等待ACK |
| `tcp.queued_events/queued_replies` | 等待送出的事件／優先回覆數，不包含正在送出的封包與telemetry |
| `tcp.telemetry_pending` | 有尚未送出的有效最新數值；每個selector只保留最新值，不是歷史讀值佇列 |
| `tcp.last_query/last_reply` | 最近0x20接收封包／實際寫出的0x20回覆；不是成對交易追蹤紀錄 |
| `tcp.rx_hex/tx_hex/rx_age_ms/tx_age_ms` | 最後收送封包與距今時間 |
| `tcp.checksum_errors/framing_errors/timeouts` | 錯誤與等待回覆逾時計數 |
| `tcp.telemetry.rpm/level/bpm` | 每項含 `value`、`age_ms`，表示最後完整寫入TCP的0x23值與距今時間 |
| `tcp.card_result` | 最近UID已送出、主機確認／拒絕、未明確確認或逾時狀態 |

0x23沒有ACK流程，因此最後送出值不代表伺服器已處理。0x25成功回覆代表接到主機指令，仍需觀察機台0x44 ACK及0x3F回報才能判斷後續狀況。刷卡傳送可能因重試而重送同一封包；既有協定沒有交易ID，主機應能處理重複資料。

設定持續顯示pending時，依序查看tracking／pulse、機台控制及UART回覆佇列、阻力等待ACK，再看TCP writing／awaiting_ack／佇列。這些欄位有助於辨別尚未套用的原因；單一瞬間所有計數為0也不代表其他工作不會立即產生新事件。

### USB／CCID

| JSON欄位 | 意義 |
|---|---|
| `rfid.state/ready/card_present` | USB／CCID工作狀態、讀卡機是否可用、卡片是否仍在場 |
| `rfid.vid/pid` | USB裝置識別，頁面轉為十六進位顯示 |
| `rfid.uid` | 最近讀到UID的完整十六進位字串，保留前導零；「最近」不等同卡片目前仍在場 |
| `rfid.cards/errors/age_ms/last_error` | 累計讀卡／錯誤、最後狀態更新距今及最近錯誤 |
| `rfid.atr` | PowerOn收到的ATR；不以單一固定ATR作為所有卡片的匹配條件 |
| `rfid.features/max_message_length` | 讀卡機描述元回報的CCID能力與訊息容量 |
| `rfid.last_sw` | 最後APDU狀態字，例如成功為 `0x9000`；與USB傳輸是否成功分開 |
| `rfid.last_tx/last_rx` | 最後CCID收送封包HEX |

## 建議除錯順序

1. 看目前入口的頁面與Heap是否持續更新，排除裝置重啟、手機換網路或STA IP變更。
2. 看STA是否有IP，再看TCP是否已連線；Wi-Fi成功不代表TCP伺服器可達。
3. BLE依序看掃描、連線、訂閱、通知計數；只有找到廣播還不能證明能取得心率。
4. UART先看RX與錯誤計數，再看0x28／0x3F各自的age，最後才判讀數值。
5. USB先看VID／PID與描述元，再看卡片在場、ATR、APDU狀態字與UID。

提交Issue時用人工資料重現或先遮蔽SSID、MAC、UID與個人讀值。不要上傳完整Flash備份或現場密碼。
