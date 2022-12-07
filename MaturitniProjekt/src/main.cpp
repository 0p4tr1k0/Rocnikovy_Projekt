#include "esp_camera.h"
#include <WiFi.h>
#include "SPIFFS.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_http_server.h"

#define PART_BOUNDARY "123456789000000000000987654321"

static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

String indexString;
#define motor1 16
#define motor2 13
#define motor3 14
#define motor4 15

//ovladač pro poslání indexu
static esp_err_t index_handler(httpd_req_t *req){
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, indexString.c_str(), indexString.length()); //pošle index z PROGMEM
}

//ovladač pro poslání obrázku z kamery
static esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE); //nataví typ odpovědi na streamovaný kontext
  if(res != ESP_OK){ //pokud nepodařilo, ukonči funkci
    return res;
  }

  while(true){
    fb = esp_camera_fb_get();
    if (!fb) { //nezískali jsme pointer na kameru
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if(fb->width > 400){ //zkompresuj každý řádek kamery do _jpg_buf
        if(fb->format != PIXFORMAT_JPEG){
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if(!jpeg_converted){
            Serial.println("JPEG compression failed");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if(res == ESP_OK){
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen); //pošle hlavičku snímku
    }
    if(res == ESP_OK){ //pošle snímek
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY)); //pošle footer snímku
    }
    if(fb){ // uvolní paměť
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if(_jpg_buf){
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if(res != ESP_OK){
      break;
    }
  }
  return res;
}

//ovladač pro pohyb
static esp_err_t go_handler(httpd_req_t *req){
  char*  buf;
  size_t buf_len = httpd_req_get_url_query_len(req) + 1;
  char variable[32] = {0,};
  
  if (buf_len > 1) {
    //vytvoření bufferu pro url
    buf = (char*)malloc(buf_len);
    if(!buf){
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) { //ziskani cele url
      if (httpd_query_key_value(buf, "direction", variable, sizeof(variable)) == ESP_OK) { //ziskani parametru
      } else {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
      }
    } else {
      free(buf);
      httpd_resp_send_404(req);
      return ESP_FAIL;
    }
    free(buf);
  } else {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  
  if(!strcmp(variable, "rovne")) {
    digitalWrite(motor3, HIGH);
    digitalWrite(motor4, LOW);
    digitalWrite(motor2, HIGH);
    digitalWrite(motor1, LOW);
  }else if(!strcmp(variable, "dozadu")) {
    digitalWrite(motor3, LOW);
    digitalWrite(motor4, HIGH);
    digitalWrite(motor2, LOW);
    digitalWrite(motor1, HIGH);
  } else if(!strcmp(variable, "vlevo")) {
    digitalWrite(motor3, LOW);
    digitalWrite(motor4, HIGH);
    digitalWrite(motor2, HIGH);
    digitalWrite(motor1, LOW);
  } else if(!strcmp(variable, "vpravo")) {
    digitalWrite(motor3, HIGH);
    digitalWrite(motor4, LOW);
    digitalWrite(motor2, LOW);
    digitalWrite(motor1, HIGH);
  } else if(!strcmp(variable, "stop")) {
    digitalWrite(motor3, LOW);
    digitalWrite(motor4, LOW);
    digitalWrite(motor2, LOW);
    digitalWrite(motor1, LOW);
  } else if(!strcmp(variable, "ledOn")) {
    digitalWrite(4, HIGH);
  } else if(!strcmp(variable, "ledOff")) {
    digitalWrite(4, LOW);
  } else {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

//nastartování serveru
void startServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  //odkaz pro index
  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };
  //odkaz pro index
  httpd_uri_t go_uri = {
    .uri       = "/go",
    .method    = HTTP_GET,
    .handler   = go_handler,
    .user_ctx  = NULL
  };
  //nastartování stránky index
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &go_uri);
  }

  //odkaz pro stream
  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };
  //nastartování stránky stream
  config.server_port = 81; //192.168.4.1:81/stream
  config.ctrl_port = 81;
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector
  
  Serial.begin(115200);
  pinMode(motor1, OUTPUT);
  pinMode(motor2, OUTPUT);
  pinMode(motor3, OUTPUT);
  pinMode(motor4, OUTPUT);
  pinMode(4, OUTPUT);

  SPIFFS.begin();
  File file = SPIFFS.open("/index.html", "r");
  while(file.available()){
    indexString = file.readString();
  }
  
  //nastavení pinů
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
  config.pin_sscb_sda = 26;
  config.pin_sscb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG; 
  
  if(psramFound()){
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }
  
  // Camera nastartování
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  //WiFi
  WiFi.softAP("Robot", "Robot123");
  delay(500);
  
  startServer();
}

void loop() {
  
}

//plán: ovládání robota, ovládání LED
