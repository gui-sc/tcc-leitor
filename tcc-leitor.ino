#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include <ESP32QRCodeReader.h>
#include "esp_http_server.h"
#include "img_converters.h"
#include "esp_timer.h"
#include "soc/rtc_cntl_reg.h"
#include <ESP32Servo.h>

// ====== Wi-Fi ======
#define WIFI_SSID ""
#define WIFI_PASS ""

// ====== HTML simples ======
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM Live + QR + Servos</title>
<style>body{margin:0;background:#111;color:#eee;font-family:system-ui,Arial}
header{padding:12px 16px;background:#222}
main{display:flex;align-items:center;justify-content:center;min-height:calc(100vh - 56px);padding:16px}
.frame{max-width:96vw;max-height:80vh;border-radius:12px;overflow:hidden;box-shadow:0 10px 30px rgba(0,0,0,.4)}
img{display:block;width:100%;height:auto;background:#000}
#qr{position:fixed;left:12px;bottom:12px;background:#000a;color:#fff;padding:8px 10px;border-radius:8px;font:12px monospace;white-space:pre-wrap}
</style></head><body>
<header><strong>ESP32-CAM • Live</strong> — <small>/qr mostra o último QR lido</small></header>
<main><div class="frame"><img src="/stream" onerror="setTimeout(()=>location.reload(),1200)"></div></main>
<pre id="qr">QR: (aguardando...)</pre>
<script>
async function tick(){
  try{
    const r = await fetch('/qr',{cache:'no-store'});
    const j = await r.json();
    const route = j.route || 'NONE';
    document.getElementById('qr').textContent =
      j.valid ? `QR: ${j.text}\n${j.age_ms} ms atrás\nroute=${route}` : 'QR: (nenhum)';
  }catch(e){}
  setTimeout(tick, 600);
}
tick();
</script>
</body></html>
)rawliteral";

// ====== Leitor de QR (inicializa a câmera) ======
ESP32QRCodeReader reader(CAMERA_MODEL_AI_THINKER);

// ====== Estado do QR ======
static QRCodeData g_qr;
static volatile bool g_has_qr = false;
static uint32_t g_last_qr_ms = 0;
static char g_last_qr_text[256];

// ====== Palavras-chave / roteamento ======
static const char* KEY_LEFT  = "ORGANICO";
static const char* KEY_RIGHT = "RECICLAVEL";
static char g_last_route[8] = "NONE"; // "LEFT" | "RIGHT" | "NONE"

// ====== Servos ======
const int SERVO_PIN  = 2; 

// Ângulos
const int SERVO_NEUTRAL     = 95;
const int SERVO_LEFT_OPEN   = 40; // esquerda "abre"
const int SERVO_RIGHT_OPEN  = 135;   // direita "abre"
const uint16_t OPEN_TIME_MS = 1800; // tempo segurando aberto

Servo servo;

void setupServoPWM() {
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
}

// ====== Qualidade do JPEG ======
static int g_jpeg_quality = 10;

static inline bool containsNoCase(const String& big, const char* key) {
  String b = big; b.toUpperCase();
  String k = String(key); k.toUpperCase();
  return b.indexOf(k) >= 0;
}

static void json_escape(const char* in, String &out) {
  for (const char* p = in; *p; ++p) {
    char c = *p;
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { out += ' '; }
    else { out += c; }
  }
}

// ====== Movimento do servo ======
void moveLeft() {
  servo.write(SERVO_LEFT_OPEN);
  delay(OPEN_TIME_MS);
  servo.write(SERVO_NEUTRAL);
}

void moveRight() {
  servo.write(SERVO_RIGHT_OPEN);
  delay(OPEN_TIME_MS);
  servo.write(SERVO_NEUTRAL);
}

void qrConsumerTask(void *pv) {
  for (;;) {
    if (reader.receiveQrCode(&g_qr, 100)) {
      if (g_qr.valid && g_qr.payload) {
        const char *p = (const char*)g_qr.payload;
        size_t n = strnlen(p, sizeof(g_last_qr_text)-1);
        memcpy((void*)g_last_qr_text, p, n);
        ((char*)g_last_qr_text)[n] = '\0';
        g_last_qr_ms = millis();
        g_has_qr = true;
        // Log no Serial
        Serial.printf("[QR] %s\n", g_last_qr_text);
      }
    }
    vTaskDelay(1);
  }
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t jpg_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }

  esp_err_t res = ESP_OK;
  if (fb->format == PIXFORMAT_JPEG) {
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  } else {
    size_t jpg_len = 0; uint8_t *jpg_buf = nullptr;
    bool ok = frame2jpg(fb, g_jpeg_quality, &jpg_buf, &jpg_len);
    if (!ok || !jpg_buf) { httpd_resp_send_500(req); esp_camera_fb_return(fb); return ESP_FAIL; }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    res = httpd_resp_send(req, (const char*)jpg_buf, jpg_len);
    free(jpg_buf);
  }
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  static const char *BOUNDARY = "frame";
  char part_buf[64];
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) break;

    esp_err_t res = httpd_resp_send_chunk(req, "\r\n--frame\r\n", 12);
    if (res != ESP_OK) { esp_camera_fb_return(fb); break; }

    if (fb->format == PIXFORMAT_JPEG) {
      int hlen = snprintf(part_buf, sizeof(part_buf),
                          "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
      esp_camera_fb_return(fb);
      if (res != ESP_OK) break;
    } else {
      size_t jpg_len = 0; uint8_t *jpg_buf = nullptr;
      bool ok = frame2jpg(fb, g_jpeg_quality, &jpg_buf, &jpg_len);
      esp_camera_fb_return(fb);
      if (!ok || !jpg_buf) break;

      int hlen = snprintf(part_buf, sizeof(part_buf),
                          "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)jpg_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)jpg_buf, jpg_len);
      free(jpg_buf);
      if (res != ESP_OK) break;
    }
    vTaskDelay(1);
  }
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

static esp_err_t qr_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  String json = "{";
  if (g_has_qr) {
    String esc; esc.reserve(strlen(g_last_qr_text)+16);
    json += "\"valid\":true,\"text\":\"";
    json_escape(g_last_qr_text, esc); json += esc;
    json += "\",\"age_ms\":"; json += String((uint32_t)(millis()-g_last_qr_ms));
    json += ",\"route\":\""; json += g_last_route; json += "\"}";
  } else {
    json += "\"valid\":false,\"route\":\""; json += g_last_route; json += "\"}";
  }
  return httpd_resp_sendstr(req, json.c_str());
}

static void start_http_server() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port   = 32768;

  httpd_handle_t server = NULL;
  if (httpd_start(&server, &cfg) == ESP_OK) {
    httpd_uri_t u1 = { .uri="/",       .method=HTTP_GET, .handler=index_handler,  .user_ctx=NULL };
    httpd_uri_t u2 = { .uri="/jpg",    .method=HTTP_GET, .handler=jpg_handler,    .user_ctx=NULL };
    httpd_uri_t u3 = { .uri="/stream", .method=HTTP_GET, .handler=stream_handler, .user_ctx=NULL };
    httpd_uri_t u4 = { .uri="/qr",     .method=HTTP_GET, .handler=qr_handler,     .user_ctx=NULL };
    httpd_register_uri_handler(server, &u1);
    httpd_register_uri_handler(server, &u2);
    httpd_register_uri_handler(server, &u3);
    httpd_register_uri_handler(server, &u4);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[ESP32-CAM] Boot");

  pinMode(4, OUTPUT); digitalWrite(4, LOW);
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.println("reader.setup()...");
  reader.setup();

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, FRAMESIZE_QVGA);
    s->set_quality(s, g_jpeg_quality);
    s->set_lenc(s, 1);
    s->set_sharpness(s, 1);
  }

  Serial.println("reader.beginOnCore(1)...");
  reader.beginOnCore(1);

  xTaskCreatePinnedToCore(qrConsumerTask, "qrConsumer", 10 * 1024, NULL, 3, NULL, 0);

  // --- Servo ---
  setupServoPWM();
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN);
  servo.write(SERVO_NEUTRAL);

  // --- Wi-Fi + HTTP ---
  WiFi.mode(WIFI_STA);

  if (strlen(WIFI_PASS) == 0) {
    WiFi.begin(WIFI_SSID);       // rede aberta (sem senha)
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASS);  // rede com senha
  }

  Serial.printf("[WiFi] Conectando em %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(500); }
  Serial.println();
  Serial.print("[WiFi] IP: "); Serial.println(WiFi.localIP());

  start_http_server();

  Serial.println("[HTTP] Acesse:");
  Serial.print("  http://"); Serial.println(WiFi.localIP());
  Serial.println("  /stream -> MJPEG");
  Serial.println("  /jpg    -> foto única");
  Serial.println("  /qr     -> último QR (JSON)");
}

const uint32_t ACTION_DEBOUNCE_MS = 2500;

void loop() {
  static String lastHandled = "";
  static uint32_t lastActionAt = 0;

  if (g_has_qr) {
    String payload = String(g_last_qr_text);
    g_has_qr = false;

    bool isNew = (payload != lastHandled) || (millis() - lastActionAt > ACTION_DEBOUNCE_MS);

    if (isNew) {
      if (containsNoCase(payload, KEY_LEFT)) {
        Serial.println("[ROUTE] ORGANICO -> LEFT (servo esquerda)");
        strcpy(g_last_route, "LEFT");
        moveLeft();
        lastHandled = payload;
        lastActionAt = millis();
      } else if (containsNoCase(payload, KEY_RIGHT)) {
        Serial.println("[ROUTE] RECICLAVEL -> RIGHT (servo direita)");
        strcpy(g_last_route, "RIGHT");
        moveRight();
        lastHandled = payload;
        lastActionAt = millis();
      } else {
        Serial.println("[ROUTE] Sem correspondência (nenhum servo acionado)");
        strcpy(g_last_route, "NONE");
      }
    }
  }

  delay(10);
}
