/*
  ESP32-CAM Surveillance — Phase 1: Live MJPEG Stream
  Board: AI-Thinker ESP32-CAM

  What this does:
    - Connects to WiFi
    - Initializes the camera (good quality: SVGA, high JPEG quality)
    - Serves an MJPEG stream at:  http://<device-ip>/stream
    - Serves a single snapshot at: http://<device-ip>/capture

*/

#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFi.h>
#include "camera_pins.h"
#include "arduino_secrets.h"

// ---------- CONFIG ----------
const char *WIFI_SSID     = SECRET_SSID;
const char *WIFI_PASSWORD = SECRET_PASS;

// Camera quality settings (tune later; SVGA is a good quality/bandwidth balance)
#define FRAME_SIZE     FRAMESIZE_SVGA   // 800x600. Options: QVGA, VGA, SVGA, XGA, UXGA...
#define JPEG_QUALITY   10               // 0-63, LOWER number = HIGHER quality (10-12 is a good start)
// -----------------------------

httpd_handle_t stream_httpd = NULL;

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
  static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
  static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
      break;
    }

    if (fb->format != PIXFORMAT_JPEG) {
      Serial.println("Non-JPEG frame, skipping");
      esp_camera_fb_return(fb);
      continue;
    }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    }

    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
  }
  return res;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  const char *html =
      "<html><body style='font-family:sans-serif;text-align:center'>"
      "<h2>ESP32-CAM Live Stream</h2>"
      "<img src='/stream' style='max-width:100%;'>"
      "<p><a href='/capture'>Take a snapshot</a></p>"
      "</body></html>";
  return httpd_resp_send(req, html, strlen(html));
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri  = {"/",        HTTP_GET, index_handler,  NULL};
  httpd_uri_t stream_uri = {"/stream",  HTTP_GET, stream_handler, NULL};
  httpd_uri_t capture_uri= {"/capture", HTTP_GET, capture_handler,NULL};

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &index_uri);
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    httpd_register_uri_handler(stream_httpd, &capture_uri);
  }
}

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // Use PSRAM (present on AI-Thinker module) for bigger frames + double buffering
  if (psramFound()) {
    config.frame_size = FRAME_SIZE;
    config.jpeg_quality = JPEG_QUALITY;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }

  // Optional: minor tuning for better image quality outdoors/indoors
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_gainceiling(s, (gainceiling_t)0);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  initCamera();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected. Camera stream ready at: http://");
  Serial.println(WiFi.localIP());

  startCameraServer();
}

void loop() {
  // Nothing needed here — the HTTP server runs in its own task.
  delay(10000);
}
