/*
  ESP32-CAM Surveillance — Phase 1 + Phase 2
  Board: AI-Thinker ESP32-CAM

  What this does:
    - Connects to WiFi
    - Initializes the camera (good quality: SVGA, high JPEG quality)
    - Serves an MJPEG stream at:      http://<device-ip>:81/stream
    - Serves a single snapshot at:    http://<device-ip>/capture
        (also auto-saves each snapshot to the onboard microSD card as img_####.jpg)
    - Lists saved photos at:          http://<device-ip>/gallery
    - Serves one saved photo at:      http://<device-ip>/photo?file=img_0001.jpg

  Requires a microSD card formatted as FAT32, inserted before power-on.

  Arduino IDE setup (one-time):
    1. File > Preferences > Additional Board Manager URLs, add:
       https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
    2. Tools > Board > Boards Manager > search "esp32" > install (Espressif Systems)
    3. Tools > Board > select "AI Thinker ESP32-CAM"
    4. Tools > Partition Scheme > "Huge APP (3MB No OTA/1MB SPIFFS)"
    5. Tools > PSRAM > "Enabled"
    6. Wiring for upload: use an FTDI/USB-TTL adapter
         FTDI TX  -> ESP32-CAM U0R (GPIO3)
         FTDI RX  -> ESP32-CAM U0T (GPIO1)
         FTDI GND -> GND
         FTDI 5V  -> 5V
         Connect GPIO0 to GND (puts it in flashing mode), press RESET, then upload.
         After upload, disconnect GPIO0 from GND and press RESET to run normally.

  Fill in your WiFi credentials below before uploading.
*/

#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFi.h>
#include "FS.h"
#include "SD_MMC.h"
#include "camera_pins.h"
#include "arduino_secrets.h"

// ---------- CONFIG ----------
const char *WIFI_SSID     = SECRET_SSID;
const char *WIFI_PASSWORD = SECRET_PASS;

// Camera quality settings (tune later; SVGA is a good quality/bandwidth balance)
#define FRAME_SIZE     FRAMESIZE_SVGA   // 800x600. Options: QVGA, VGA, SVGA, XGA, UXGA...
#define JPEG_QUALITY   10               // 0-63, LOWER number = HIGHER quality (10-12 is a good start)
// -----------------------------

httpd_handle_t control_httpd = NULL;  // serves "/" and "/capture"
httpd_handle_t stream_httpd  = NULL;  // serves "/stream" only (its own server so it never blocks the others)

bool sdCardAvailable = false;   // set true in initSDCard() if a card is found
uint32_t photoCounter = 0;      // next photo number to use, e.g. img_0007.jpg

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

  if (sdCardAvailable) {
    char filename[32];
    snprintf(filename, sizeof(filename), "/img_%04u.jpg", photoCounter);
    File file = SD_MMC.open(filename, FILE_WRITE);
    if (file) {
      file.write(fb->buf, fb->len);
      file.close();
      Serial.printf("Saved %s (%u bytes)\n", filename, fb->len);
      photoCounter++;
    } else {
      Serial.printf("Failed to open %s for writing\n", filename);
    }
  }

  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// Lists every saved .jpg on the SD card as a clickable link.
static esp_err_t gallery_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");

  if (!sdCardAvailable) {
    const char *msg = "<html><body><h3>No SD card detected.</h3></body></html>";
    return httpd_resp_send(req, msg, strlen(msg));
  }

  String html = "<html><body style='font-family:sans-serif'><h2>Saved Photos</h2><ul>";
  File root = SD_MMC.open("/");
  File entry = root.openNextFile();
  while (entry) {
    String name = String(entry.name());
    if (name.endsWith(".jpg")) {
      String fname = name.substring(1); // strip the leading '/'
      html += "<li><a href='/photo?file=" + fname + "' target='_blank'>" + fname +
              "</a> (" + String(entry.size() / 1024) + " KB)</li>";
    }
    entry = root.openNextFile();
  }
  html += "</ul><p><a href='/'>&laquo; Back to live view</a></p></body></html>";
  return httpd_resp_send(req, html.c_str(), html.length());
}

// Streams one saved photo back, e.g. GET /photo?file=img_0003.jpg
static esp_err_t photo_handler(httpd_req_t *req) {
  char query[64];
  char filename[40] = "";

  if (httpd_req_get_url_query_len(req) > 0 &&
      httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "file", filename, sizeof(filename));
  }

  if (strlen(filename) == 0) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  char path[64];
  snprintf(path, sizeof(path), "/%s", filename);

  File file = SD_MMC.open(path, FILE_READ);
  if (!file) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  uint8_t buf[1024];
  size_t bytesRead;
  while ((bytesRead = file.read(buf, sizeof(buf))) > 0) {
    if (httpd_resp_send_chunk(req, (const char *)buf, bytesRead) != ESP_OK) {
      file.close();
      return ESP_FAIL;
    }
  }
  httpd_resp_send_chunk(req, NULL, 0); // signals end of chunked response
  file.close();
  return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  // Note: the stream lives on a SEPARATE server/port (81) from this page (80),
  // so that a long-running stream connection never blocks /capture requests.
  char html[512];
  snprintf(html, sizeof(html),
      "<html><body style='font-family:sans-serif;text-align:center'>"
      "<h2>ESP32-CAM Live Stream</h2>"
      "<img src='http://%s:81/stream' style='max-width:100%%;'>"
      "<p><a href='/capture' target='_blank'>Take a snapshot</a> | "
      "<a href='/gallery'>View saved photos</a></p>"
      "</body></html>", WiFi.localIP().toString().c_str());
  return httpd_resp_send(req, html, strlen(html));
}

void startCameraServer() {
  // --- Control server: port 80, handles "/" and "/capture" ---
  httpd_config_t control_config = HTTPD_DEFAULT_CONFIG();
  control_config.server_port = 80;
  control_config.ctrl_port = 32768;

  httpd_uri_t index_uri   = {"/",        HTTP_GET, index_handler,   NULL};
  httpd_uri_t capture_uri = {"/capture", HTTP_GET, capture_handler, NULL};
  httpd_uri_t gallery_uri = {"/gallery", HTTP_GET, gallery_handler, NULL};
  httpd_uri_t photo_uri   = {"/photo",   HTTP_GET, photo_handler,   NULL};

  if (httpd_start(&control_httpd, &control_config) == ESP_OK) {
    httpd_register_uri_handler(control_httpd, &index_uri);
    httpd_register_uri_handler(control_httpd, &capture_uri);
    httpd_register_uri_handler(control_httpd, &gallery_uri);
    httpd_register_uri_handler(control_httpd, &photo_uri);
  }

  // --- Stream server: port 81, handles "/stream" only ---
  // Kept on its own server/task so its blocking loop never starves /capture.
  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;

  httpd_uri_t stream_uri = {"/stream", HTTP_GET, stream_handler, NULL};

  if (httpd_start(&stream_httpd, &stream_config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
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

void initSDCard() {
  // "true" = 1-bit mode. The AI-Thinker board's SD data-line 1 shares GPIO4
  // with the onboard flash LED, so 1-bit mode avoids that pin conflict entirely.
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD card mount failed");
    sdCardAvailable = false;
    return;
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("No SD card detected");
    sdCardAvailable = false;
    return;
  }

  sdCardAvailable = true;
  Serial.println("SD card mounted");

  // Resume numbering after whatever photos already exist, instead of
  // restarting at img_0001.jpg and overwriting old ones on every reboot.
  uint32_t maxIndex = 0;
  File root = SD_MMC.open("/");
  File entry = root.openNextFile();
  while (entry) {
    String name = String(entry.name());
    if (name.startsWith("/img_") && name.endsWith(".jpg")) {
      uint32_t idx = name.substring(5, name.length() - 4).toInt();
      if (idx > maxIndex) maxIndex = idx;
    }
    entry = root.openNextFile();
  }
  photoCounter = maxIndex + 1;
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  initCamera();
  initSDCard();

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
