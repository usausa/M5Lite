#include <M5Unified.h>
#include <WiFi.h>
#include <FastLED.h>

// =====================================================================
// Setting
// =====================================================================
static constexpr const char* WIFI_SSID       = "xxxxxx";
static constexpr const char* WIFI_PASSWORD   = "xxxxxxxx";
static constexpr uint8_t     STATIC_IP[]     = {192, 168, 100, 100};
static constexpr uint8_t     GATEWAY_IP[]    = {192, 168, 100,   1};
static constexpr uint8_t     SUBNET_IP[]     = {255, 255, 255, 0};
static constexpr int         TCP_PORT        = 10000;

// SK6812 LED Unit (GPIO26)
// LED: 0=Red, 1=Yellow, 2=Green
static constexpr int     LED_PIN        = 26;
static constexpr int     LED_COUNT      = 3;
static constexpr uint8_t LED_BRIGHTNESS = 50;
static constexpr int     BLINK_INTERVAL = 500;  // ms

// =====================================================================

struct PatliteStatus {
    bool greenBlink  = false;
    bool yellowBlink = false;
    bool redBlink    = false;
    int  buzzer      = 0;
    bool green       = false;
    bool yellow      = false;
    bool red         = false;
};

// =====================================================================

CRGB leds[LED_COUNT];
PatliteStatus currentStatus;
WiFiServer server(TCP_PORT);
bool blinkOn = false;
unsigned long lastBlink = 0;

// =====================================================================

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

static CRGB lampColor(bool steady, bool blink, CRGB color) {
    if (steady)           return color;
    if (blink && blinkOn) return color;
    return CRGB::Black;
}

static void updateLEDs() {
    leds[0] = lampColor(currentStatus.red,    currentStatus.redBlink,    CRGB::Red);
    leds[1] = lampColor(currentStatus.yellow, currentStatus.yellowBlink, CRGB::Yellow);
    leds[2] = lampColor(currentStatus.green,  currentStatus.greenBlink,  CRGB::Green);
    FastLED.show();
}

static void updateBlink() {
    unsigned long now = millis();
    if (now - lastBlink >= BLINK_INTERVAL) {
        blinkOn   = !blinkOn;
        lastBlink = now;
        updateLEDs();
    }
}

static void handleClient(WiFiClient& client) {
    while (client.connected()) {
        updateBlink();
        M5.update();

        if (!client.available()) {
            delay(1);
            continue;
        }

        uint8_t cmd = client.read();

        if (cmd == 'W') {
            // Wait status (timeout 1s)
            unsigned long deadline = millis() + 1000;
            while (!client.available() && millis() < deadline) { delay(1); }

            if (client.available()) {
                uint8_t statusByte = client.read();
                statusFromByte(currentStatus, statusByte);
                updateLEDs();
                client.write("ACK", 3);
                M5.Log.printf("W status=0x%02X -> ACK\n", statusByte);
            } else {
                client.write("NACK", 4);
                M5.Log.println("W timeout -> NACK");
            }
        } else if (cmd == 'R') {
            uint8_t resp[2] = { 'R', statusToByte(currentStatus) };
            client.write(resp, 2);
            M5.Log.printf("R status=0x%02X\n", resp[1]);
        } else {
            client.write("NACK", 4);
            M5.Log.printf("Unknown cmd=0x%02X -> NACK\n", cmd);
        }
    }
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    FastLED.addLeds<SK6812, LED_PIN, GRB>(leds, LED_COUNT);
    FastLED.setBrightness(LED_BRIGHTNESS);
    FastLED.clear(true);

    // WiFi connect
    WiFi.config(
        IPAddress(STATIC_IP[0],  STATIC_IP[1],  STATIC_IP[2],  STATIC_IP[3]),
        IPAddress(GATEWAY_IP[0], GATEWAY_IP[1], GATEWAY_IP[2], GATEWAY_IP[3]),
        IPAddress(SUBNET_IP[0],  SUBNET_IP[1],  SUBNET_IP[2],  SUBNET_IP[3])
    );
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    M5.Log.print("Connecting to WiFi");

    int waitCount = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        leds[1] = (waitCount++ % 2 == 0) ? CRGB::Yellow : CRGB::Black;
        FastLED.show();
        M5.Log.print(".");
        if (waitCount > 40) {
            M5.Log.println(" TIMEOUT!");
            leds[0] = CRGB::Red;
            leds[1] = CRGB::Black;
            leds[2] = CRGB::Black;
            FastLED.show();
            return;
        }
    }

    FastLED.clear(true);
    M5.Log.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    server.begin();
    M5.Log.printf("TCP server listening on port %d\n", TCP_PORT);

    // Connect complete
    leds[2] = CRGB::Green;
    FastLED.show();
    delay(1000);
    FastLED.clear(true);
}

void loop() {
    M5.update();
    updateBlink();

    WiFiClient client = server.available();
    if (client) {
        M5.Log.printf("Client connected: %s\n", client.remoteIP().toString().c_str());
        handleClient(client);
        client.stop();
        M5.Log.println("Client disconnected");
        updateLEDs();
    }
}
