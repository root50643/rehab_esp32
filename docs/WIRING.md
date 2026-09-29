# 接線與供電

這份文件描述韌體使用的功能接線。它不是特定開發板的插座定位圖，也不代表目前所有線路已接妥。實測項目見 [VALIDATION.md](VALIDATION.md)。

![功能接線圖](images/wiring.svg)

## 預設接腳

| 功能 | GPIO／介面 | 方向（以 ESP32 為準） | 說明 |
|---|---|---|---|
| 機台 UART TX | GPIO17 | 輸出 | 接機台 RX |
| 機台 UART RX | GPIO18 | 輸入 | 接機台 TX |
| Start | GPIO4 | 輸出 | 開機 LOW；啟動 HIGH 500 ms 後回 LOW |
| Stop | GPIO5 | 輸出 | 開機 LOW；停止 HIGH 3000 ms 後 LOW，等待500 ms才完成流程 |
| USB D− | GPIO19 | 雙向 | ESP32-S3 原生 USB Host |
| USB D+ | GPIO20 | 雙向 | ESP32-S3 原生 USB Host |
| USB VBUS | 適當5V電源路徑 | 供電 | 需依讀卡機與板子電路配置保護、共地與反灌防護 |
| 燒錄／除錯 | CH340 USB-UART | 雙向 | 電腦目前識別為 COM3；序列監看115200 |

GPIO17／18／4／5 可修改 `firmware/RehabEsp32/src/board_config.h` 後重編譯。USB19／20 是原生 USB 腳位，不能比照一般 UART 任意搬移。此版本針對 ESP32-S3；不是為 ESP32、ESP32-C3 或 ESP32-S2 通用設計。

## 機台 UART

- 設定為 **57600 baud、8資料位、無同位元檢查、1停止位元（8N1）**。
- TX 接對方 RX，RX 接對方 TX；不得 TX 接 TX。
- 先確認機台 UART 的實際電壓、邏輯極性與介面規格。原 Raspberry Pi 接線不等於 ESP32 能直接沿用。
- 如果是3.3V TTL且雙方介面相容，可依電路設計共地直連；若是5V TTL，需合適電位轉換；若是RS-232，需對應收發器，不能直接接GPIO。
- 若現場採隔離 UART，訊號地與電源分區依隔離器規格連接，勿用額外地線破壞隔離。

開始時先驗證機台自發回報，觀察進階資訊中的 UART RX／checksum 錯誤；尚未確認啟停電路前，不透過主機發出控制命令。

## Start／Stop 控制

韌體輸出的是 GPIO 邏輯脈衝。GPIO 不直接驅動馬達、繼電器線圈或未知的機台按鈕電路；沿用經確認的驅動介面，或依機台規格設計光耦／電晶體／繼電器驅動等必要電路。

必須先確認 HIGH 的有效極性、需要的電壓／電流、共地或隔離方式，以及脈衝是否符合實機輸入。文件沒有指定未經量測的限流電阻、電晶體或繼電器型號。

Start 脈衝完成後開始追蹤數值。Stop 的3秒 HIGH 與500 ms等待完成後停止追蹤，之後才可能套用待處理設定。原暫停命令僅回覆，不產生GPIO脈衝。

## CL-2000 USB Host

讀卡機必須接在 **ESP32-S3 原生 USB Host 路徑**，不是電腦上的USB埠，也不是板上的 CH340 資料介面。韌體只支援 VID `0x1206`／PID `0x2107` 且描述元符合 CCID APDU 能力的讀卡機。

| USB 訊號 | 接法 |
|---|---|
| D− | ESP32-S3 GPIO19 ↔ 讀卡機 USB D− |
| D+ | ESP32-S3 GPIO20 ↔ 讀卡機 USB D+ |
| VBUS | 經核對電流容量與保護的5V供電 ↔ 讀卡機 VBUS |
| GND | ESP32、USB介面與5V供電的對應參考地 |

D+/D− 應使用適當的 USB 連接器及短配線，保持USB訊號品質。若開發板原生插座已連到GPIO19／20，可使用正確的Host轉接方案；是否具備VBUS輸出、電流限制、方向切換與ESD元件，須查該板原理圖，不能由「有USB-C插座」推論。

**勿由3.3V腳或GPIO供電給USB讀卡機；勿直接把兩個未隔離的5V來源互接。** ESP32 上的USB Host資料功能不會自動替外接讀卡機建立安全5V供電。電腦透過CH340供電時，需確認新增USB Host電源不反灌。

原生USB不可同時作為USB Host與本韌體的CDC／MSC／DFU裝置。工具鏈將這些開機功能停用，並以編譯期檢查防止誤設。實作使用 [ESP-IDF USB Host API](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/usb_host.html)。

## 建議實機接線順序

1. 只接CH340到電腦，備份、燒錄，確認設定AP與頁面。
2. 在未連接復健機前，量測 GPIO4／5 的開機低電位與預期脈衝時序。
3. 確認UART電氣規格，再接GPIO17／18及所需地線／介面，觀察回報。
4. 確認Start／Stop驅動介面及有效極性後接上控制訊號，再做空載或受控實機驗收。
5. 確認USB Host5V供電與反灌防護，接CL-2000，驗證描述元、讀卡、移除與重插。

記錄開發板標示、接線照片、介面電路、供電方式與測試結果；公開前遮蔽裝置識別資料。此紀錄是把「預設接法」提升為「已驗證接法」的依據。
