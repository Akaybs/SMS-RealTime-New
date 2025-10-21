#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <vector>

// === Cấu hình Wi-Fi / Firebase / SIM ===
#define WIFI_SSID "HOANG WIFI"
#define WIFI_PASSWORD "hhhhhhhh"

#define Web_API_KEY "AIzaSyD-f2CMpJkrXrjttgoPAouLPQon4jd5PWE"
#define DATABASE_URL "https://hoanglsls-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define USER_EMAIL "akaybs@gmail.com"
#define USER_PASS "chxhcnvn"

#define LED_PIN 2

// SIM A800
HardwareSerial sim800(1);
#define SIM800_TX 17
#define SIM800_RX 16
#define SIM800_BAUD 115200
bool simReady = false;

// Firebase
UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);
FirebaseApp app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database;

// LED status
unsigned long lastBlinkTime = 0;
bool ledState = false;
int ledMode = 0; // 1 wifi err, 2 firebase err, 3 sim err, 4 ok

// Outbox struct and queue
struct OutgoingSMS {
  String id;        // firebase key, e.g. "2363"
  String phone;
  String smsMain;
  String smsDebt;   // optional continuation
  bool isProcessing = false;
};
std::vector<OutgoingSMS> outbox;

// ==================== HÀM TIỆN ÍCH ====================
String removeVietnameseAccents(String str)
{
  const char *find[] = {"á","à","ả","ã","ạ","ă","ắ","ằ","ẳ","ẵ","ặ","â","ấ","ầ","ẩ","ẫ","ậ",
                        "đ",
                        "é","è","ẻ","ẽ","ẹ","ê","ế","ề","ể","ễ","ệ",
                        "í","ì","ỉ","ĩ","ị",
                        "ó","ò","ỏ","õ","ọ","ô","ố","ồ","ổ","ỗ","ộ","ơ","ớ","ờ","ở","ỡ","ợ",
                        "ú","ù","ủ","ũ","ụ","ư","ứ","ừ","ử","ữ","ự",
                        "ý","ỳ","ỷ","ỹ","ỵ",
                        "Á","À","Ả","Ã","Ạ","Ă","Ắ","Ằ","Ẳ","Ẵ","Ặ","Â","Ấ","Ầ","Ẩ","Ẫ","Ậ",
                        "Đ",
                        "É","È","Ẻ","Ẽ","Ẹ","Ê","Ế","Ề","Ể","Ễ","Ệ",
                        "Í","Ì","Ỉ","Ĩ","Ị",
                        "Ó","Ò","Ỏ","Õ","Ọ","Ô","Ố","Ồ","Ổ","Ỗ","Ộ","Ơ","Ớ","Ờ","Ở","Ỡ","Ợ",
                        "Ú","Ù","Ủ","Ũ","Ụ","Ư","Ứ","Ừ","Ử","Ữ","Ự",
                        "Ý","Ỳ","Ỷ","Ỹ","Ỵ"};
  const char *repl[] = {"a","a","a","a","a","a","a","a","a","a","a","a","a","a","a","a","a",
                        "d",
                        "e","e","e","e","e","e","e","e","e","e","e",
                        "i","i","i","i","i",
                        "o","o","o","o","o","o","o","o","o","o","o","o","o","o","o","o","o",
                        "u","u","u","u","u","u","u","u","u","u","u",
                        "y","y","y","y","y",
                        "A","A","A","A","A","A","A","A","A","A","A","A","A","A","A","A","A",
                        "D",
                        "E","E","E","E","E","E","E","E","E","E","E",
                        "I","I","I","I","I",
                        "O","O","O","O","O","O","O","O","O","O","O","O","O","O","O","O","O",
                        "U","U","U","U","U","U","U","U","U","U","U",
                        "Y","Y","Y","Y","Y"};
  for (int i = 0; i < (int)(sizeof(find) / sizeof(find[0])); i++)
    str.replace(find[i], repl[i]);
  return str;
}

String formatMoney(long money)
{
  String s = String(money);
  String res = "";
  int len = s.length();
  int count = 0;
  for (int i = len - 1; i >= 0; i--)
  {
    res = s.charAt(i) + res;
    count++;
    if (count == 3 && i != 0)
    {
      res = "." + res;
      count = 0;
    }
  }
  return res;
}

// LED update
void updateLed()
{
  unsigned long now = millis();
  if (ledMode == 1)
  {
    if (now - lastBlinkTime > 1000)
    {
      lastBlinkTime = now;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
  else if (ledMode == 2)
  {
    if (now - lastBlinkTime > 250)
    {
      lastBlinkTime = now;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
  else if (ledMode == 3)
  {
    static int blinkCount = 0;
    static unsigned long patternTimer = 0;
    if (now - patternTimer > 200)
    {
      patternTimer = now;
      blinkCount++;
      digitalWrite(LED_PIN, blinkCount % 2);
      if (blinkCount >= 4)
      {
        blinkCount = 0;
        delay(1600);
      }
    }
  }
  else if (ledMode == 4)
  {
    digitalWrite(LED_PIN, LOW);
  }
}

// ==================== GỬI SMS VỚI PHẢN HỒI CHÍNH XÁC ====================
bool sendSMS(String phone, String content)
{
  // 🔹 Xóa buffer UART cũ (tránh sót phản hồi từ lệnh trước)
  while (sim800.available()) sim800.read();

  // 🔹 Đặt chế độ text mode
  sim800.println("AT+CMGF=1");
  delay(300);
  if (!sim800.find("OK"))
  {
    Serial.println("⚠️ [SIM800] Không phản hồi sau AT+CMGF=1");
  }

  // 🔹 Gửi lệnh bắt đầu tin nhắn
  sim800.print("AT+CMGS=\"");
  sim800.print(phone);
  sim800.println("\"");
  delay(300);

  // 🔹 Gửi nội dung tin nhắn
  sim800.print(content);
  sim800.write(26); // Ctrl+Z (kết thúc nội dung SMS)

  // 🔹 Đọc phản hồi từ SIM800
  String response = "";
  unsigned long start = millis();
  while (millis() - start < 8000) // chờ tối đa 8 giây
  {
    while (sim800.available())
    {
      char c = sim800.read();
      response += c;
    }

    // ✅ Tin nhắn đã gửi thành công
    if (response.indexOf("+CMGS:") != -1)
    {
      Serial.println("✅ [SIM800] SMS gửi thành công");
      Serial.println("📥 [Phản hồi] " + response);
      return true;
    }

    // ❌ Gửi thất bại
    if (response.indexOf("ERROR") != -1)
    {
      Serial.println("❌ [SIM800] SMS gửi thất bại");
      Serial.println("📥 [Phản hồi] " + response);
      return false;
    }
  }

  // ⚠️ Quá thời gian chờ, không có phản hồi
  Serial.println("⚠️ [SIM800] Không nhận phản hồi trong thời gian chờ");
  Serial.println("📥 [Phản hồi cuối] " + response);
  return false;
}


// Cập nhật trạng thái SMS lên Firebase (gọi từ loop())
void updateSMSStatus(String id, String status)
{
  String path = "/roitai/" + id + "/sms";
  // Gọi Database.set trong ngữ cảnh loop (không phải trong processData callback)
  Database.set<String>(aClient, path, status, [](AsyncResult &r)
  {
    if (r.isError())
      Firebase.printf("❌ Update error: %s\n", r.error().message().c_str());
    else
      Firebase.printf("✅ Updated %s\n", r.c_str());
  });
}

// ==================== FIREBASE CALLBACK (chỉ push vào outbox) ====================
void processData(AsyncResult &aResult)
{
  if (!aResult.isResult()) return;
  if (!aResult.available()) return;

  Firebase.printf("📥 [Phan hoi API] %s\n", aResult.c_str());

  // Dùng DynamicJsonDocument với dung lượng đủ lớn
  // Nếu JSON lớn hơn, tăng con số này.
  const size_t CAP = 8192;
  DynamicJsonDocument doc(CAP);
  DeserializationError err = deserializeJson(doc, aResult.c_str());
  if (err)
  {
    Serial.print("deserializeJson error: ");
    Serial.println(err.c_str());
    return;
  }

  JsonObject root = doc.as<JsonObject>();
  for (JsonPair kv : root)
  {
    JsonObject item = kv.value().as<JsonObject>();

    // id là key của object, ví dụ "2363"
    String id = String(kv.key().c_str());

    String name = item.containsKey("name") ? item["name"].as<String>() : "";
    String phone = item.containsKey("phone") ? item["phone"].as<String>() : "";
    String iphone = item.containsKey("iphone") ? item["iphone"].as<String>() : "";
    String imei = item.containsKey("imei") ? item["imei"].as<String>() : "";
    String loi = item.containsKey("loi") ? item["loi"].as<String>() : "";
    String thanhtoan = item.containsKey("thanhtoan") ? item["thanhtoan"].as<String>() : "";
    String smsStatus = item.containsKey("sms") ? item["sms"].as<String>() : "";
    String thoigian = item.containsKey("thoigian") ? item["thoigian"].as<String>() : "";
    String tienText = item.containsKey("tienText") ? item["tienText"].as<String>() : "";
    int soLuongNo = item.containsKey("soLuongNo") ? item["soLuongNo"].as<int>() : 0;
    long totalDebtVal = item.containsKey("totalDebt") ? item["totalDebt"].as<long>() : 0;

    // Chuẩn hóa tiền tệ
    tienText.replace("₫", "VND");
    tienText.replace("đ", "VND");
    tienText.replace(" vnđ", "VND");
    tienText.replace("VNĐ", "VND");

    loi.replace("₫", "VND");
    loi.replace("đ", "VND");
    loi.replace(" vnđ", "VND");
    loi.replace("VNĐ", "VND");

    String smsContent = "";

    if (smsStatus == "Send" && thanhtoan == "TT")
    {
      smsContent = "TB: " + name + "\nBan da " + loi + "\nTime: " + thoigian + "\nhttps://hoanglsls.web.app";
    }
    else if (thanhtoan == "Ok" && smsStatus == "Yes")
    {
      smsContent = "TB:" + id + " " + name + "\nTHANH TOAN OK\n" + iphone + "\nImei: " + imei +
                   "\nLoi: " + loi + "\nTien: " + tienText + "\n" + thoigian + "\n";
      if (soLuongNo >= 1 && totalDebtVal > 0)
        smsContent += "Tong no (" + String(soLuongNo) + " may): " + formatMoney(totalDebtVal) + " VND\n";
    }
    else if (thanhtoan == "Nợ" && smsStatus == "Yes")
    {
      smsContent = "ID:" + id + " " + name + "\nCHUA THANH TOAN\n" + iphone + "\nImei: " + imei +
                   "\nLoi: " + loi + "\nTien: " + tienText + "\n" + thoigian + "\n";
      if (soLuongNo >= 1 && totalDebtVal > 0)
        smsContent += "Tong no (" + String(soLuongNo) + " may): " + formatMoney(totalDebtVal) + " VND\n";
    }

    if (smsContent.length() > 0)
    {
      smsContent = removeVietnameseAccents(smsContent);

      // Tách nếu quá dài (giữ logic cũ)
      int pos = smsContent.indexOf("Tong no");
      String smsMain = smsContent;
      String smsDebt = "";
      if (pos != -1 && smsContent.length() > 160)
      {
        smsMain = smsContent.substring(0, pos);
        smsMain.trim();
        smsDebt = smsContent.substring(pos);
        smsDebt.trim();
      }

      // Đẩy vào outbox (không thao tác mạng ở đây)
      OutgoingSMS o;
      o.id = id;
      o.phone = phone;
      o.smsMain = smsMain;
      o.smsDebt = smsDebt;
      o.isProcessing = false;
      outbox.push_back(o);

      Serial.println("🗂️ Pushed to outbox: ID=" + id + " phone=" + phone);
    }
  }

  // Lưu ý: doc sẽ giải phóng khi hàm kết thúc, nhưng chúng ta đã copy mọi thứ vào String nên an toàn.
}

// ==================== KIỂM TRA SIM A800 ====================
bool checkSimModule()
{
  sim800.println("AT");
  unsigned long t = millis();
  while (millis() - t < 1000)
  {
    if (sim800.find("OK"))
      return true;
  }
  return false;
}

// ==================== SETUP ====================
void setup()
{
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  sim800.begin(SIM800_BAUD, SERIAL_8N1, SIM800_RX, SIM800_TX);
  Serial.println("🔹 SIM A800 initializing...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    ledMode = 1;
    updateLed();
    delay(300);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());

  if (checkSimModule())
  {
    simReady = true;
    Serial.println("✅ SIM A800 OK");
  }
  else
  {
    simReady = false;
    Serial.println("❌ SIM A800 ERROR");
  }

  ssl_client.setInsecure();
  ssl_client.setHandshakeTimeout(10);

  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  Serial.println("🚀 System Ready!");
}

// ==================== LOOP ====================
void loop()
{
  app.loop();
  updateLed();

  if (WiFi.status() != WL_CONNECTED)
    ledMode = 1;
  else if (!app.ready())
    ledMode = 2;
  else if (!simReady)
    ledMode = 3;
  else
    ledMode = 4;

  // Xử lý outbox theo thứ tự FIFO. Chỉ xử lý 1 mục tại một thời điểm.
  static unsigned long lastProcess = 0;
  if (!outbox.empty() && (millis() - lastProcess > 1000))
  {
    lastProcess = millis();
    // Lấy phần tử đầu
    OutgoingSMS o = outbox.front();
    outbox.erase(outbox.begin()); // remove first

    Serial.println("📌 [Xử lý outbox] ID=" + o.id + " phone=" + o.phone);
    Serial.println("📌 [Xem truoc SMS 1]\n" + o.smsMain);

    bool success = sendSMS(o.phone, o.smsMain);
    String newStatus = success ? "Done" : "Error";
    updateSMSStatus(o.id, newStatus);
    delay(3000); // đợi 3s trước khi gửi phần tiếp theo

    if (o.smsDebt.length() > 0)
    {
      Serial.println("📌 [Xem truoc SMS 2]\n" + o.smsDebt);
      bool s2 = sendSMS(o.phone, o.smsDebt);
      // bạn có thể decide có cập nhật trạng thái lần 2 hay không; ở đây giữ nguyên trạng thái trước đó
      (void)s2;
      delay(2000);
    }
  }

  // Polling Firebase mỗi 15s (như cũ)
  static unsigned long last = 0;
  if (app.ready() && millis() - last > 15000)
  {
    last = millis();
    Database.get(aClient, "/roitai", processData, false, "GetRoitaiData");
  }
}
