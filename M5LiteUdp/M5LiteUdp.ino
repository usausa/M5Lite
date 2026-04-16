#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <FastLED.h>

// =====================================================================
// 設定(変更箇所)
// =====================================================================
static constexpr const char* WIFI_SSID       = "xxxxxx";
static constexpr const char* WIFI_PASSWORD   = "xxxxxxxx";
static constexpr uint8_t     STATIC_IP[]     = {192, 168, 100, 100};
static constexpr uint8_t     GATEWAY_IP[]    = {192, 168, 100,   1};
static constexpr uint8_t     SUBNET_IP[]     = {255, 255, 255, 0};
static constexpr int         UDP_PORT        = 10000;

// =====================================================================
// 定義
// =====================================================================

// SK6812 LEDユニット設定(Groveポート: GPIO26)
// LEDインデックス: 0=赤, 1=黄, 2=緑
static constexpr int     LED_PIN        = 26;
static constexpr int     LED_COUNT      = 3;
static constexpr uint8_t LED_BRIGHTNESS = 50;
static constexpr int     BLINK_INTERVAL = 500;  // 点滅間隔 (ms)

// パトライトの点灯状態
struct PatliteStatus {
    bool greenBlink  = false;  // 緑点滅
    bool yellowBlink = false;  // 黄点滅
    bool redBlink    = false;  // 赤点滅
    int  buzzer      = 0;      // ブザー(2ビット値: 0-3)
    bool green       = false;  // 緑点灯
    bool yellow      = false;  // 黄点灯
    bool red         = false;  // 赤点灯
};

// =====================================================================
// 状態
// =====================================================================

WiFiUDP       udp;               // UDPソケット
CRGB          leds[LED_COUNT];   // FastLED描画バッファ
PatliteStatus currentStatus;     // 現在のパトライト状態
bool          blinkOn   = false; // 点滅トグル(true=点灯フェーズ)
unsigned long lastBlink = 0;     // 前回トグル時刻(ms)

// =====================================================================
// ヘルパー
// =====================================================================

// PatliteStatusを1バイトへシリアライズする
// ビット割り当て: [7]=緑点滅 [6]=黄点滅 [5]=赤点滅 [4-3]=ブザー [2]=緑 [1]=黄 [0]=赤
// ブザー自体は外部ユニットが必要なので未サポート、状態の管理のみ
static uint8_t statusToByte(const PatliteStatus& s) {
    uint8_t b = 0;
    if (s.greenBlink)    b |= 0b10000000;
    if (s.yellowBlink)   b |= 0b01000000;
    if (s.redBlink)      b |= 0b00100000;
    if (s.buzzer & 0b10) b |= 0b00010000;
    if (s.buzzer & 0b01) b |= 0b00001000;
    if (s.green)         b |= 0b00000100;
    if (s.yellow)        b |= 0b00000010;
    if (s.red)           b |= 0b00000001;
    return b;
}

// 1バイトをPatliteStatusへデシリアライズする
static void statusFromByte(PatliteStatus& s, uint8_t b) {
    s.greenBlink  = (b & 0b10000000) != 0;
    s.yellowBlink = (b & 0b01000000) != 0;
    s.redBlink    = (b & 0b00100000) != 0;
    s.buzzer      = 0;
    if (b & 0b00010000) s.buzzer += 2;
    if (b & 0b00001000) s.buzzer += 1;
    s.green  = (b & 0b00000100) != 0;
    s.yellow = (b & 0b00000010) != 0;
    s.red    = (b & 0b00000001) != 0;
}

// steadyがtrue、または点滅フェーズならcolorを、それ以外は黒を返す
static CRGB lampColor(bool steady, bool blink, CRGB color) {
    if (steady)           return color;
    if (blink && blinkOn) return color;
    return CRGB::Black;
}

// currentStatusの内容をLEDバッファに反映して表示する
static void updateLEDs() {
    leds[0] = lampColor(currentStatus.red,    currentStatus.redBlink,    CRGB::Red);
    leds[1] = lampColor(currentStatus.yellow, currentStatus.yellowBlink, CRGB::Yellow);
    leds[2] = lampColor(currentStatus.green,  currentStatus.greenBlink,  CRGB::Green);
    FastLED.show();
}

// パトライト状態をリセットしてLEDを消灯する
static void resetStatus() {
    currentStatus = PatliteStatus{};
    updateLEDs();
}

// BLINK_INTERVAL(ms)ごとに点滅トグルを切り替え、LEDを再描画する
static void updateBlink() {
    unsigned long now = millis();
    if (now - lastBlink >= BLINK_INTERVAL) {
        blinkOn   = !blinkOn;
        lastBlink = now;
        updateLEDs();
    }
}

// =====================================================================
// パケット処理
// =====================================================================

// プロトコル:
//   'W' + <1byte>  ステータス書き込み → "ACK"
//   'R'            ステータス読み出し → 'R' + <1byte>
//   その他         → "NACK"
static void handlePacket() {
    int len = udp.parsePacket();
    if (len <= 0) return;

    uint8_t buf[2];
    int n = udp.read(buf, sizeof(buf));
    if (n <= 0) return;

    uint8_t cmd = buf[0];

    if (cmd == 'W') {
        // データ受信
        if (n >= 2) {
            statusFromByte(currentStatus, buf[1]);
            updateLEDs();
            udp.beginPacket(udp.remoteIP(), udp.remotePort());
            udp.write((const uint8_t*)"ACK", 3);
            udp.endPacket();
        } else {
            udp.beginPacket(udp.remoteIP(), udp.remotePort());
            udp.write((const uint8_t*)"NACK", 4);
            udp.endPacket();
        }
    } else if (cmd == 'R') {
        // 現在のステータスを返す
        uint8_t resp[2] = { 'R', statusToByte(currentStatus) };
        udp.beginPacket(udp.remoteIP(), udp.remotePort());
        udp.write(resp, 2);
        udp.endPacket();
    } else {
        udp.beginPacket(udp.remoteIP(), udp.remotePort());
        udp.write((const uint8_t*)"NACK", 4);
        udp.endPacket();
    }
}

// =====================================================================
// ハンドラー
// =====================================================================

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    // LED ユニット初期化・消灯
    FastLED.addLeds<SK6812, LED_PIN, GRB>(leds, LED_COUNT);
    FastLED.setBrightness(LED_BRIGHTNESS);
    FastLED.clear(true);

    // WiFi 接続
    WiFi.config(
        IPAddress(STATIC_IP[0],  STATIC_IP[1],  STATIC_IP[2],  STATIC_IP[3]),
        IPAddress(GATEWAY_IP[0], GATEWAY_IP[1], GATEWAY_IP[2], GATEWAY_IP[3]),
        IPAddress(SUBNET_IP[0],  SUBNET_IP[1],  SUBNET_IP[2],  SUBNET_IP[3])
    );
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // 接続完了または20秒タイムアウトまで待機
    int waitCount = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        if (++waitCount > 40) return;
    }

    // UDPソケット開始
    udp.begin(UDP_PORT);
}

void loop() {
    M5.update();
    updateBlink();

    // ボタン押下でパトライト状態をリセット
    if (M5.BtnA.wasPressed()) {
        resetStatus();
    }

    // 受信パケットを処理する
    handlePacket();
}
