#include <M5Unified.h>
#include <NTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <time.h>
#include <vector> //動的配列用のライブラリ
#include <SD.h>
#include <SPI.h>


#define SD_SPI_CS_PIN   4
#define SD_SPI_SCK_PIN  18
#define SD_SPI_MISO_PIN 38
#define SD_SPI_MOSI_PIN 23

#define SAVEINTERVAL 300  // CSV保存間隔（秒）


//------------------------------------
// Wi-Fi設定
//------------------------------------
const char* ssid = "SSID";
const char* password = "PASSWORD";
const char* GAS_URL = "URL";

// NTP
const long gmtOffset_sec = 9*3600;      // UTCとの時差（秒単位）又はJSTの場合は9*3600　
const int daylightOffset_sec = 0;   // サマータイムは使用しない
const char* ntpServer = "ntp.nict.jp"; // NICT（日本の標準サーバー）
const char* ntpServer2 = "time.google.com";

//------------------------------------
// UDP設定
//------------------------------------
const unsigned int UDP_PORT = 16520;
char udpBuffer[512];  // XML受信バッファ

//------------------------------------
// 構造体定義
//------------------------------------
struct UecsData {
  char type[20];
  int room;
  int region;
  int order;
  int priority;
  float data;
  char ip[16];
  time_t timestamp;  // UTC保持 time_t型はUnix時間を扱うための型（1970年1月1日からの経過秒数）
  char timeStr[25];  // ローカル時刻の整形済み文字列 "%Y-%m-%d %H:%M:%S"
};

//------------------------------------
// グローバル
//------------------------------------
WiFiUDP udp;
std::vector<UecsData> uecsBuffer;  // 5分間のバッファ　動的配列の宣言 <UecsData型>の配列 uecsBuffer
time_t lastSaveTime = 0;           // 最終CSV保存時刻

//------------------------------------
// 初期化関数
//------------------------------------
//----------Wi-Fi接続------------------------------
void connectWiFi() {
  Serial.printf("Connecting to Wi-Fi SSID: %s\n", ssid);
  M5.Display.printf("Connecting to Wi-Fi SSID: %s\n", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWi-Fi connected. IP: %s\n", WiFi.localIP().toString().c_str());
  M5.Display.printf("Wi-Fi connected. IP: %s\n", WiFi.localIP().toString().c_str());
}

//----------NTPサーバーと時刻同期------------------------------
void setupNTP() {
    Serial.print("Configuring NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, ntpServer2); // タイムゾーン、サマータイムオフセット、NTPサーバーを設定
    time_t now = 0;
    const time_t LIMIT_TIME_SEC = 1672531200; // 2023年1月1日 00:00:00のタイムスタンプ

    while ( now < LIMIT_TIME_SEC ) {  // time(nullptr) が妥当な時刻を示すまで待機
      delay(500);
        now = time(nullptr);
        Serial.print(".");
    }

    struct tm timeinfo;
    localtime_r(&now, &timeinfo); //localtime_r は、time_t 型の値を、年、月、日、時、分、秒などの個々の要素に分解した struct tm 構造体に変換
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y/%m/%d %H:%M:%S", &timeinfo); // strftimeで時刻を整形して表示
    Serial.print("\nNTP done: ");
    Serial.println(timeStr);
    M5.Display.printf("NTP done: %s\n", timeStr);
    
}

//----------SDカード初期化------------------------------
bool initSD() {
  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    Serial.println("SD init failed");
    M5.Display.println("SD init failed");
    return false;
  }
  Serial.println("SD_MMC initialized");
  return true;
}

//------------------------------------
// 属性抽出ヘルパー　<DATA type="..." room="...."...> から属性値を取得
//------------------------------------
String extractAttr(const String& tag, const String& attrName) { // attrNameは"type"や"room"
  String key = attrName + "=\"";         // 例: keyに「type="」を代入
  int start = tag.indexOf(key);          // 「type="」の開始位置を検索、startに代入
  if (start < 0) return "";              // 見つからなければ(-1)空文字列を返す
  start += key.length();                 // keyの長さを加え、値の開始位置をstartに代入
  int end = tag.indexOf("\"", start);    // start以降で最初の「"」を検索、endに代入
  if (end < 0) return "";                // 見つからなければ空文字列を返す 
  return tag.substring(start, end);      // startからendの手前までの部分文字列を返す
}

//------------------------------------
// XML解析関数　例）<DATA type="SoilTemp.mIC" room="1" region="1" order="1" priority="15">23.5</DATA>
//------------------------------------
bool parseUecsXml(const String& xml, UecsData &out) {
  int dataStart = xml.indexOf("<DATA");                       // 「<DATA」の開始位置を検索
  int dataEnd   = xml.indexOf("</DATA>");                     // 「</DATA>」の開始位置を検索
  if (dataStart < 0 || dataEnd < 0) return false;             // 見つからなければ解析失敗でfalseを返す

  String dataTag = xml.substring(dataStart, dataEnd);         // dataTagに「<DATA」と「</DATA>」の間の文字列を抽出
  String type = extractAttr(dataTag, "type");                 // ectractAttr関数を使って、type属性値を抽出

  // cnD.* のタイプはスキップする
  if (type.startsWith("cnd.")) return false;
  
  // 「.m??」形式でなければ無視
  //int dotPos = type.indexOf(".m");                            // 計測機器系ノードを示す「.m」の位置を検索。DATA typeは、「xx___x.mNN」の命名規則で計測機器系ノードの識別を行う 
  //if (dotPos < 0 || type.length() < dotPos + 3) return false; // 見つからなければ解析失敗でfalseを返す

  strncpy(out.type, type.c_str(), sizeof(out.type));          // out.typeにSringオブジェクトのtype属性値をout.typeのサイズ分だけコピー
  out.type[sizeof(out.type) - 1] = '\0';                      // 【追加】念のため、最後にnull終端を追加
  out.room     = extractAttr(dataTag, "room").toInt();        // out.roomにroom属性値を整数に変換して代入
  out.region   = extractAttr(dataTag, "region").toInt();      // out.regionにregion属性値を整数に変換して代入
  out.order    = extractAttr(dataTag, "order").toInt();       // out.orderにorder属性値を整数に変換して代入
  out.priority = extractAttr(dataTag, "priority").toInt();    // out.priorityにpriority属性値を整数に変換して代入

  int close = xml.indexOf(">", dataStart);                    // dataTagの閉じ「>」の位置を検索  
  String dataValue = xml.substring(close + 1, dataEnd);       // dataValueに「>」の次の位置から「</DATA>」の手前までの部分文字列を抽出
  out.data = dataValue.toFloat();                             // out.dataにdataValueを浮動小数点数に変換して代入

  int ipStart = xml.indexOf("<IP>");                          // 「<IP>」の開始位置を検索  
  int ipEnd   = xml.indexOf("</IP>");                         // 「</IP>」の開始位置を検索
  if (ipStart >= 0 && ipEnd > ipStart)                        // 見つかれば
    strncpy(out.ip, xml.substring(ipStart + 4, ipEnd).c_str(), sizeof(out.ip)); // out.ipにIPアドレス部分文字列をout.ipのサイズ分だけコピー
  else
    strcpy(out.ip, "0.0.0.0");

  out.timestamp = time(nullptr);                              // UTCの現在時刻を取得してout.timestampに代入

  // ここでローカル時刻文字列を作成（JST）
  time_t localTime = out.timestamp;
  struct tm tm_info;
  localtime_r(&localTime, &tm_info);
  strftime(out.timeStr, sizeof(out.timeStr), "%Y-%m-%d %H:%M:%S", &tm_info);

  return true;
}

//------------------------------------
// 受信データをバッファに格納（重複上書き）
//------------------------------------
void storeUecsData(const UecsData& newData) {
  for (auto &d : uecsBuffer) {                    // 動的配列uecsBuffer内のすべての要素を走査
    if (strcmp(d.type, newData.type) == 0 &&      // strcmp()でtype文字列を比較
        d.room == newData.room &&                 // 他の属性値も比較 ]
        d.region == newData.region &&
        d.order == newData.order &&
        d.priority == newData.priority) {
      d = newData;                                // type,room,region,order,priorityが一致したら既存の要素dに新しいデータnewDataを上書き  
      return;
    }
  }
  uecsBuffer.push_back(newData);                  //重複する要素がない場合、動的配列の末尾に要素を追加 push_back()
}

//------------------------------------
// Googleスプレッドシートへ追記保存（1エントリ分）
//------------------------------------
void sendToGoogle(const UecsData &d) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(GAS_URL);
  http.addHeader("Content-Type", "application/json");

  char payload[384];
  snprintf(payload, sizeof(payload),
    "{\"timestamp\":%ld,\"timeStr\":\"%s\",\"type\":\"%s\",\"room\":%d,"
    "\"region\":%d,\"order\":%d,\"priority\":%d,"
    "\"data\":%.2f,\"ip\":\"%s\"}",
    d.timestamp, d.timeStr, d.type, d.room, d.region,
    d.order, d.priority, d.data, d.ip);

  int code = http.POST(payload);
  Serial.printf("POST -> %d\n", code);
  http.end();
}

//------------------------------------
// CSVへ追記保存（1エントリ分）
//------------------------------------
void saveToCsv(const UecsData &d) {
  File file = SD.open("/uecs_log.csv", FILE_APPEND);
  if (!file) {
    M5.Display.println("Failed to open CSV");
    Serial.println("Failed to open CSV");
    return;
  }

  // parseUecsXml で作成済みの日時文字列をそのまま使用
  Serial.printf("Saving: %s,%s,%d,%d,%d,%d,%.2f,%s\n",
                d.timeStr, d.type, d.room, d.region, d.order,
                d.priority, d.data, d.ip);

  M5.Display.printf("Saving: %s,%s,%d,%d,%d,%d,%.2f,%s\n",
                d.timeStr, d.type, d.room, d.region, d.order,
                d.priority, d.data, d.ip);

  file.printf("%s,%s,%d,%d,%d,%d,%.2f,%s\n",
              d.timeStr, d.type, d.room, d.region, d.order,
              d.priority, d.data, d.ip);

  file.close();
}


//------------------------------------
// バッファの各エントリを処理（保存 + Google送信）
//------------------------------------
void processBufferEntries() {
  for (auto &d : uecsBuffer) {
    saveToCsv(d);
    sendToGoogle(d);
  }
  uecsBuffer.clear();
  lastSaveTime = time(nullptr);
}

//---------UDP受信、解析、バッファ格納------------------------------
void handleUdp() {
  int packetSize = udp.parsePacket();                     // 受信パケットサイズを取得   
  if (packetSize) {
    int len = udp.read(udpBuffer, sizeof(udpBuffer)-1);   // 受信データをudpBufferに読み込み
    if (len > 0) {                            
      udpBuffer[len] = 0;                                 // 文字列終端のnullを追加
      String xml = String(udpBuffer);                     // Stringオブジェクトに変換
      Serial.printf("Received %d bytes from %s:\n%s\n", len, udp.remoteIP().toString().c_str(), xml.c_str());

      UecsData d;                                         // UecsData構造体のインスタンスdを宣言
      if (parseUecsXml(xml, d)) {                         // XML解析に成功したら  parseUecsXml関数を呼び出し
        storeUecsData(d);                                 // 受信データをバッファに格納 storeUecsData関数を呼び出し
        Serial.printf("Stored: %s %.2f\n", d.type, d.data);
      }
    }
  }
}

//------------------------------------
// setup
//------------------------------------
void setup() {
  Serial.begin(115200);
  delay(100);
  M5.begin();
  M5.Display.setTextFont(2);
  M5.Display.setTextScroll(true);
  M5.Display.println("Starting UECS Recorder");

  connectWiFi();
  setupNTP();
  lastSaveTime = time(nullptr);
  udp.begin(UDP_PORT);
  initSD(); //SDカード初期化
}

//------------------------------------
// loop
//------------------------------------
void loop() {
  handleUdp();

  // 定期的に値を保存
  time_t now = time(nullptr);
  if (now - lastSaveTime >= SAVEINTERVAL) {
    Serial.println("Saving ...");
    processBufferEntries(); // 変更点：個別保存ループを呼び出す
  }
  delay(10); // 適度に待つ
}
