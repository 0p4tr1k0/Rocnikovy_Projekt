#include "esp_camera.h"
#include <WiFi.h>
#include "SPIFFS.h"
#include "Arduino.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_http_server.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

#define PART_BOUNDARY "123456789000000000000987654321"

static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

String indexString;
#define motor1 13
#define motor2 15
#define motor3 14
#define motor4 2

// ovladač pro poslání obrázku z kamery
static esp_err_t stream_handler(httpd_req_t *req)
{
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char *part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE); // nataví typ odpovědi na streamovaný kontext
  if (res != ESP_OK)
  { // pokud nepodařilo, ukonči funkci
    return res;
  }

  while (true)
  {
    fb = esp_camera_fb_get();
    if (!fb)
    { // nezískali jsme pointer na kameru
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    }
    else
    {
      if (fb->width > 400)
      { // zkompresuj každý řádek kamery do _jpg_buf
        if (fb->format != PIXFORMAT_JPEG)
        {
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if (!jpeg_converted)
          {
            Serial.println("JPEG compression failed");
            res = ESP_FAIL;
          }
        }
        else
        {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if (res == ESP_OK)
    {
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen); // pošle hlavičku snímku
    }
    if (res == ESP_OK)
    { // pošle snímek
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK)
    {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY)); // pošle footer snímku
    }
    if (fb)
    { // uvolní paměť
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    }
    else if (_jpg_buf)
    {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK)
    {
      break;
    }
  }
  return res;
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len)
{
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
  {
    data[len] = 0;
    if (!strcmp((char *)data, "rovne"))
    {
      digitalWrite(motor3, HIGH);
      digitalWrite(motor4, LOW);
      digitalWrite(motor2, HIGH);
      digitalWrite(motor1, LOW);
    }
    else if (!strcmp((char *)data, "dozadu"))
    {
      digitalWrite(motor3, LOW);
      digitalWrite(motor4, HIGH);
      digitalWrite(motor2, LOW);
      digitalWrite(motor1, HIGH);
    }
    else if (!strcmp((char *)data, "vlevo"))
    {
      digitalWrite(motor3, LOW);
      digitalWrite(motor4, HIGH);
      digitalWrite(motor2, HIGH);
      digitalWrite(motor1, LOW);
    }
    else if (!strcmp((char *)data, "vpravo"))
    {
      digitalWrite(motor3, HIGH);
      digitalWrite(motor4, LOW);
      digitalWrite(motor2, LOW);
      digitalWrite(motor1, HIGH);
    }
    else if (!strcmp((char *)data, "stop"))
    {
      digitalWrite(motor3, LOW);
      digitalWrite(motor4, LOW);
      digitalWrite(motor2, LOW);
      digitalWrite(motor1, LOW);
    }
    else if (!strcmp((char *)data, "ledOn"))
    {
      digitalWrite(4, HIGH);
    }
    else if (!strcmp((char *)data, "ledOff"))
    {
      digitalWrite(4, LOW);
    }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
    Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
    break;
  case WS_EVT_DISCONNECT:
    Serial.printf("WebSocket client #%u disconnected\n", client->id());
    digitalWrite(motor3, LOW);
    digitalWrite(motor4, LOW);
    digitalWrite(motor2, LOW);
    digitalWrite(motor1, LOW);
    digitalWrite(4, LOW);
    break;
  case WS_EVT_DATA:
    handleWebSocketMessage(arg, data, len);
    break;
  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  }
}

void setup()
{
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout detector

  Serial.begin(115200);
  pinMode(motor1, OUTPUT);
  pinMode(motor2, OUTPUT);
  pinMode(motor3, OUTPUT);
  pinMode(motor4, OUTPUT);
  pinMode(4, OUTPUT);

  // získaní stránky z spiffs
  SPIFFS.begin();
  File file = SPIFFS.open("/index.html", "r");
  while (file.available())
  {
    indexString = file.readString();
  }

  // nastavení pinů
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound())
  {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  }
  else
  {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  // Camera nastartování
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
  // Camera převrácení obrazu
  sensor_t *s = esp_camera_sensor_get()
                    s->set_vflip(s, 1);
  // WiFi
  WiFi.softAP("Robot", "Robot123");
  delay(500);

  // stream server
  httpd_config_t configWeb = HTTPD_DEFAULT_CONFIG();
  configWeb.server_port = 81; // 192.168.4.1:81/stream
  configWeb.ctrl_port = 81;

  // odkaz pro stream
  httpd_uri_t stream_uri = {
      .uri = "/stream",
      .method = HTTP_GET,
      .handler = stream_handler,
      .user_ctx = NULL};
  // nastartování stránky stream
  if (httpd_start(&stream_httpd, &configWeb) == ESP_OK)
  {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }

  // web server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/html", indexString); });
  server.begin();

  // websocket server
  ws.onEvent(onEvent);
  server.addHandler(&ws);
}

void loop()
{
  ws.cleanupClients();
}

// credits:
// streamování https://randomnerdtutorials.com/esp32-cam-video-streaming-web-server-camera-home-assistant/
// websocket https://randomnerdtutorials.com/esp32-websocket-server-arduino/