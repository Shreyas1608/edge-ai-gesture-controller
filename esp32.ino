#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

// ==========================================
// 1. NETWORK CREDENTIALS
// ==========================================
const char* ssid = "your ssid";      // Your hotspot SSID
const char* password = "your password"; // Your hotspot password

// ==========================================
// 2. HARDWARE PIN DEFINITIONS
// ==========================================
// Actuators & Indicators
#define LED_GREEN         12
#define LED_FLASH          4
#define SERVO_PIN         13

// Servo PWM Parameters (Compatible with ESP32 Core v3.x)
#define SERVO_LEDC_FREQ    50      // Standard 50Hz PWM for analog servos
#define SERVO_LEDC_RES     14      // 14-bit resolution (0 - 16383)

// Camera Pins (AI-THINKER Configuration)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ==========================================
// 3. SERVER & STREAM DEFINITIONS
// ==========================================
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t control_httpd = NULL;

// Helper function to write servo angles via LEDC
void writeServoAngle(int angle) {
  angle = constrain(angle, 0, 180);
  // Standard 50Hz servo timing: 0.5ms (0 deg) to 2.4ms (180 deg) in a 20ms period
  // 14-bit resolution: 2^14 = 16384 levels
  // 0.5ms = (0.5 / 20.0) * 16384 ≈ 410 counts
  // 2.4ms = (2.4 / 20.0) * 16384 ≈ 1966 counts
  uint32_t duty = 410 + ((angle * (1966 - 410)) / 180);
  ledcWrite(SERVO_PIN, duty);
}

// Handler for MJPEG Video Stream (Port 81)
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    }

    if (fb->format != PIXFORMAT_JPEG) {
      bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
      esp_camera_fb_return(fb);
      fb = NULL;
      if (!jpeg_converted) {
        res = ESP_FAIL;
        break;
      }
    } else {
      _jpg_buf_len = fb->len;
      _jpg_buf = fb->buf;
    }

    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }

    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }

    if (res != ESP_OK) break;
  }
  return res;
}

// Handler for LED Control: /led?pin=12&state=1
static esp_err_t led_handler(httpd_req_t *req) {
  char* buf;
  size_t buf_len;
  char pin_str[8] = {0};
  char state_str[8] = {0};

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char*)malloc(buf_len);
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      if (httpd_query_key_value(buf, "pin", pin_str, sizeof(pin_str)) == ESP_OK &&
          httpd_query_key_value(buf, "state", state_str, sizeof(state_str)) == ESP_OK) {
        
        int pin = atoi(pin_str);
        int state = atoi(state_str);

        if (pin == LED_GREEN || pin == LED_FLASH) {
          digitalWrite(pin, state ? HIGH : LOW);
        }
      }
    }
    free(buf);
  }

  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

// Handler for Servo Movement: /servo?angle=90
static esp_err_t servo_handler(httpd_req_t *req) {
  char* buf;
  size_t buf_len;
  char angle_str[8] = {0};

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char*)malloc(buf_len);
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      if (httpd_query_key_value(buf, "angle", angle_str, sizeof(angle_str)) == ESP_OK) {
        int angle = atoi(angle_str);
        writeServoAngle(angle);
      }
    }
    free(buf);
  }

  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "OK", 2);
  return ESP_OK;
}

// Start HTTP Server with Endpoints
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t led_uri = {
    .uri       = "/led",
    .method    = HTTP_GET,
    .handler   = led_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t servo_uri = {
    .uri       = "/servo",
    .method    = HTTP_GET,
    .handler   = servo_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&control_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(control_httpd, &led_uri);
    httpd_register_uri_handler(control_httpd, &servo_uri);
  }

  // Stream Server on Port 81
  config.server_port = 81;
  config.ctrl_port = 81;
  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

// ==========================================
// 4. SETUP & INITIALIZATION
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize Discrete Outputs
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_FLASH, OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_FLASH, LOW);

  // Initialize Servo via ESP32 Core v3 API (No ledcSetup or ledcAttachPin)
  ledcAttach(SERVO_PIN, SERVO_LEDC_FREQ, SERVO_LEDC_RES);
  writeServoAngle(90); // Default neutral position

  // Configure Camera Parameters
  camera_config_t cam_config;
  cam_config.ledc_channel = LEDC_CHANNEL_0;
  cam_config.ledc_timer = LEDC_TIMER_0;
  cam_config.pin_d0 = Y2_GPIO_NUM;
  cam_config.pin_d1 = Y3_GPIO_NUM;
  cam_config.pin_d2 = Y4_GPIO_NUM;
  cam_config.pin_d3 = Y5_GPIO_NUM;
  cam_config.pin_d4 = Y6_GPIO_NUM;
  cam_config.pin_d5 = Y7_GPIO_NUM;
  cam_config.pin_d6 = Y8_GPIO_NUM;
  cam_config.pin_d7 = Y9_GPIO_NUM;
  cam_config.pin_xclk = XCLK_GPIO_NUM;
  cam_config.pin_pclk = PCLK_GPIO_NUM;
  cam_config.pin_vsync = VSYNC_GPIO_NUM;
  cam_config.pin_href = HREF_GPIO_NUM;
  cam_config.pin_sscb_sda = SIOD_GPIO_NUM;
  cam_config.pin_sscb_scl = SIOC_GPIO_NUM;
  cam_config.pin_pwdn = PWDN_GPIO_NUM;
  cam_config.pin_reset = RESET_GPIO_NUM;
  cam_config.xclk_freq_hz = 20000000;
  cam_config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    cam_config.frame_size = FRAMESIZE_VGA;  // 640x480
    cam_config.jpeg_quality = 12;
    cam_config.fb_count = 2;
  } else {
    cam_config.frame_size = FRAMESIZE_CIF;  // 400x296
    cam_config.jpeg_quality = 12;
    cam_config.fb_count = 1;
  }

  // Camera Initialization
  esp_err_t err = esp_camera_init(&cam_config);
  if (err != ESP_OK) {
    Serial.printf("Camera initialization failed with error 0x%x\n", err);
    return;
  }

  // Connect to Wi-Fi
  Serial.printf("Connecting to Wi-Fi: %s", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Connected!");

  // Start HTTP Services
  startCameraServer();

  Serial.println("========================================");
  Serial.print("Camera Stream URL : http://");
  Serial.print(WiFi.localIP());
  Serial.println(":81/stream");
  Serial.print("Control Endpoint  : http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
  Serial.println("========================================");
}

void loop() {
  delay(10000);
}