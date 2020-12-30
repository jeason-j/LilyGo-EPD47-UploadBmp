
#include <esp_task_wdt.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include "epd_driver.h"
#include "webpages.h"


const String ssid = "CMCC-r3Ff";
const String password = "9999900000";
const int default_webserverporthttp = 80;
const String global_bmp = "/tmp.bmp";

#define FILESYSTEM SPIFFS
uint8_t *framebuffer;

static const uint16_t input_buffer_pixels = 20;       // may affect performance
static const uint16_t max_palette_pixels = 256;       // for depth <= 8
uint8_t mono_palette_buffer[max_palette_pixels / 8];  // palette buffer for depth <= 8 b/w
uint8_t color_palette_buffer[max_palette_pixels / 8]; // palette buffer for depth <= 8 c/w
uint8_t input_buffer[3 * input_buffer_pixels];        // up to depth 24

bool rebootESP_flag = false;
bool sleep_flag = false;

String listFiles(bool ishtml = false);
uint32_t handleUpload_start = 0;

AsyncWebServer *server;               // initialise webserver


uint16_t read16(File &f)
{
  // BMP data is stored little-endian, same as Arduino.
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read(); // LSB
  ((uint8_t *)&result)[1] = f.read(); // MSB
  return result;
}

uint32_t read32(File &f)
{
  // BMP data is stored little-endian, same as Arduino.
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read(); // LSB
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read(); // MSB
  return result;
}

void drawBitmap(const char *filename, int16_t x, int16_t y, bool with_color)
{
  File file;
  bool valid = false; // valid format to be handled
  bool flip = true;   // bitmap is stored bottom-to-top
  uint32_t startTime = millis();

  //待显示图像大于显示屏区域
  if ((x >= EPD_WIDTH) || (y >= EPD_HEIGHT))
    return;

  Serial.println();
  Serial.print("Loading image '");
  Serial.print(filename);
  Serial.println('\'');

  file = FILESYSTEM.open(filename, FILE_READ);
  if (!file) {
    Serial.print("File not found");
    return;
  }

  uint16_t filehead = read16(file);
  Serial.println("filehead:");
  Serial.println(filehead, HEX);

  //1.遇到BMP文件头
  // Parse BMP header
  if (filehead == 0x4D42) {
    // BMP signature
    uint32_t fileSize = read32(file);
    uint32_t creatorBytes = read32(file);
    uint32_t imageOffset = read32(file); // Start of image data
    uint32_t headerSize = read32(file);
    uint32_t width = read32(file);
    uint32_t height = read32(file);
    uint16_t planes = read16(file);
    uint16_t depth = read16(file); // bits per pixel
    uint32_t format = read32(file);
    if ((planes == 1) && ((format == 0) || (format == 3))) {
      // uncompressed is handled, 565 also
      Serial.print("File size: ");
      Serial.println(fileSize);
      Serial.print("Image Offset: ");
      Serial.println(imageOffset);
      Serial.print("Header size: ");
      Serial.println(headerSize);
      Serial.print("Bit Depth: ");
      Serial.println(depth);
      Serial.print("Image size: ");
      Serial.print(width);
      Serial.print('x');
      Serial.println(height);
      // BMP rows are padded (if needed) to 4-byte boundary
      uint32_t rowSize = (width * depth / 8 + 3) & ~3;
      if (depth < 8)
        rowSize = ((width * depth + 8 - depth) / 8 + 3) & ~3;
      if (height < 0) {
        height = -height;
        flip = false;
      }
      uint16_t w = width;
      uint16_t h = height;
      if ((x + w - 1) >= EPD_WIDTH)
        w = EPD_WIDTH - x;
      if ((y + h - 1) >= EPD_HEIGHT)
        h = EPD_HEIGHT - y;
      valid = true;
      uint8_t bitmask = 0xFF;
      uint8_t bitshift = 8 - depth;
      uint16_t red, green, blue;
      bool whitish, colored;

      //分析色深，如果色深为1，为无彩色模式
      if (depth == 1)
        with_color = false;
      if (depth <= 8) {
        if (depth < 8)
          bitmask >>= depth;
        file.seek(54); //palette is always @ 54
        //调色板
        for (uint16_t pn = 0; pn < (1 << depth); pn++) {
          blue = file.read();
          green = file.read();
          red = file.read();
          file.read();
          whitish = with_color ? ((red > 0x80) && (green > 0x80) && (blue > 0x80)) : ((red + green + blue) > 3 * 0x80); // whitish
          colored = (red > 0xF0) || ((green > 0xF0) && (blue > 0xF0));                                                  // reddish or yellowish?
          if (0 == pn % 8)
            mono_palette_buffer[pn / 8] = 0;
          mono_palette_buffer[pn / 8] |= whitish << pn % 8;
          if (0 == pn % 8)
            color_palette_buffer[pn / 8] = 0;
          color_palette_buffer[pn / 8] |= colored << pn % 8;
        }
      }
      //清空面板
      //display.fillScreen(GxEPD_WHITE);
      //memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

      uint32_t rowPosition = flip ? imageOffset + (height - h) * rowSize : imageOffset;
      //2.显示数据
      //2.1 按行
      for (uint16_t row = 0; row < h; row++, rowPosition += rowSize) {
        // for each line
        uint32_t in_remain = rowSize;
        uint32_t in_idx = 0;
        uint32_t in_bytes = 0;
        uint8_t in_byte = 0; // for depth <= 8
        uint8_t in_bits = 0; // for depth <= 8
        uint16_t color = 0xFF;  //默认白色
        file.seek(rowPosition);
        //2.2 按列
        for (uint16_t col = 0; col < w; col++) {
          // for each pixel
          // Time to read more pixel data?
          if (in_idx >= in_bytes) {
            // ok, exact match for 24bit also (size IS multiple of 3)
            in_bytes = file.read(input_buffer, in_remain > sizeof(input_buffer) ? sizeof(input_buffer) : in_remain);
            in_remain -= in_bytes;
            in_idx = 0;
          }

          //2.3 分析色深，不同色深获取数据规则不一样
          // 色彩值放入 colored 变量
          switch (depth) {
            case 24:
              blue = input_buffer[in_idx++];
              green = input_buffer[in_idx++];
              red = input_buffer[in_idx++];
              whitish = with_color ? ((red > 0x80) && (green > 0x80) && (blue > 0x80)) : ((red + green + blue) > 3 * 0x80); // whitish
              colored = (red > 0xF0) || ((green > 0xF0) && (blue > 0xF0));                                                  // reddish or yellowish?
              break;
            case 16: {
                uint8_t lsb = input_buffer[in_idx++];
                uint8_t msb = input_buffer[in_idx++];
                if (format == 0) {
                  // 555
                  blue = (lsb & 0x1F) << 3;
                  green = ((msb & 0x03) << 6) | ((lsb & 0xE0) >> 2);
                  red = (msb & 0x7C) << 1;
                } else {
                  // 565
                  blue = (lsb & 0x1F) << 3;
                  green = ((msb & 0x07) << 5) | ((lsb & 0xE0) >> 3);
                  red = (msb & 0xF8);
                }
                whitish = with_color ? ((red > 0x80) && (green > 0x80) && (blue > 0x80)) : ((red + green + blue) > 3 * 0x80); // whitish
                colored = (red > 0xF0) || ((green > 0xF0) && (blue > 0xF0));                                                  // reddish or yellowish?
              }
              break;
            case 1:
            case 4:
            case 8: {
                if (0 == in_bits) {
                  in_byte = input_buffer[in_idx++];
                  in_bits = 8;
                }
                uint16_t pn = (in_byte >> bitshift) & bitmask;
                whitish = mono_palette_buffer[pn / 8] & (0x1 << pn % 8);
                colored = color_palette_buffer[pn / 8] & (0x1 << pn % 8);
                in_byte <<= depth;
                in_bits -= depth;
              }
              break;
          }

          if (whitish) {
            color = 0xFF;
          } else if (colored && with_color) {
            color = 0;
          } else {
            //黑色
            color = 0;
          }


          uint16_t yrow = y + (flip ? h - row - 1 : row);
          //2.4 将这个点进行色彩显示
          epd_draw_pixel(x + col, yrow, color, framebuffer);
        } // end pixel
      }     // end line
      Serial.print("loaded in ");
      Serial.print(millis() - startTime);
      Serial.println(" ms");
    }
  }

  //3.数据读取完毕，结束退出
  file.close();
  if (!valid) {
    Serial.println("bitmap format not handled.");
  }
}

void screen_drawBitmap(String fn)
{
  epd_poweron();
  volatile uint32_t t1 = millis();
  epd_clear();
  volatile uint32_t t2 = millis();
  printf("EPD clear took %dms.\n", t2 - t1);
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  //fn = "/" + fn;
  drawBitmap(fn.c_str(), 0, 0, false);
  epd_draw_grayscale_image(epd_full_screen(), framebuffer);

  epd_poweroff();
}


String processor(const String& var) {
  if (var == "FILELIST") {
    return listFiles(true);
  }
  if (var == "FREESPIFFS") {
    return humanReadableSize((SPIFFS.totalBytes() - SPIFFS.usedBytes()));
  }

  if (var == "USEDSPIFFS") {
    return humanReadableSize(SPIFFS.usedBytes());
  }

  if (var == "TOTALSPIFFS") {
    return humanReadableSize(SPIFFS.totalBytes());
  }

  return String();
}

void rebootESP() {
  Serial.print("Rebooting ESP32: ");
  ESP.restart();
}

// list all of the files, if ishtml=true, return html rather than simple text
String listFiles(bool ishtml) {
  String returnText = "";
  Serial.println("Listing files stored on SPIFFS");
  File root = SPIFFS.open("/");
  File foundfile = root.openNextFile();
  if (ishtml) {
    returnText += "<table><tr><th align='left'>Name</th><th align='left'>Size</th></tr>";
  }
  while (foundfile) {
    if (ishtml) {
      returnText += "<tr align='left'><td><a href='" + String(foundfile.name()) + "' target='_blank'> " +    String(foundfile.name()) +
                    "</a></td><td>" + humanReadableSize(foundfile.size()) + "</td></tr>";
    }
    else
    {
      returnText += "File: " + String(foundfile.name()) + "\n";
    }
    foundfile = root.openNextFile();
  }
  if (ishtml) {
    returnText += "</table>";
  }
  root.close();
  foundfile.close();
  return returnText;
}

// Make size of files human readable
// source: https://github.com/CelliesProjects/minimalUploadAuthESP32
String humanReadableSize(const size_t bytes) {

  /*
    if (bytes < 1024) return String(bytes) + " B";
    else if (bytes < (1024 * 1024)) return String(bytes / 1024.0) + " KB";
    else if (bytes < (1024 * 1024 * 1024)) return String(bytes / 1024.0 / 1024.0) + " MB";
    else return String(bytes / 1024.0 / 1024.0 / 1024.0) + " GB";
  */
  return String(bytes) + " Bytes";
}


String getContentType(String filename) {
  //if(server->hasArg("download")) return "application/octet-stream";
  if (filename.endsWith(".htm")) return "text/html";
  else if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".png")) return "image/png";
  else if (filename.endsWith(".gif")) return "image/gif";
  else if (filename.endsWith(".jpg")) return "image/jpeg";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".xml")) return "text/xml";
  else if (filename.endsWith(".pdf")) return "application/x-pdf";
  else if (filename.endsWith(".zip")) return "application/x-zip";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  else if (filename.endsWith(".bmp")) return "application/x-bmp";
  return "text/plain";
}


//无法返回图片信息，原因？
bool handleFileRead(AsyncWebServerRequest * request)
{
  String path = request->url();
  path.toLowerCase();
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  Serial.println("path=" + path);
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
    if (SPIFFS.exists(pathWithGz))
      path += ".gz";
    //File file = SPIFFS.open(path, "r");
    Serial.println("path=" + path + ",contentType=" + contentType);
    //size_t sent = server.streamFile(file, contentType);
     request->send(SPIFFS, path, contentType);
    //request->send(SPIFFS, global_bmp, "application/x-bmp");
    //request->send(SPIFFS, global_bmp, "application/x-bmp");
    //file.close();
    return true;
  }
  return false;
}

void notFound(AsyncWebServerRequest *request) //回调函数
{
  if (!handleFileRead(request))
    request->send(404, "text/plain", "FileNotFound");

}

void configureWebServer() {
  server->on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    String logmessage = "Client:" + request->client()->remoteIP().toString() + + " " + request->url();
    Serial.println(logmessage);
    request->send_P(200, "text/html", index_html, processor);
  });

  server->on("/showbmp", HTTP_GET, [](AsyncWebServerRequest * request) {
    String logmessage = "Client:" + request->client()->remoteIP().toString() + + " " + request->url();
    Serial.println(logmessage);
    request->send(SPIFFS, global_bmp, "application/x-bmp");
  });

  server->on("/sleep", HTTP_GET, [](AsyncWebServerRequest * request) {
    String logmessage = "Client:" + request->client()->remoteIP().toString() + + " " + request->url();
    Serial.println(logmessage);
    sleep_flag = true;
    Serial.println("timer sleep");
  });

  server->on("/reboot", HTTP_GET, [](AsyncWebServerRequest * request) {
    String logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url();
    request->send(200, "text/html", reboot_html);
    Serial.println(logmessage);
    rebootESP_flag = true;
    Serial.println("timer rebootESP");
  });

  // run handleUpload function when any file is uploaded
  server->on("/upload", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(200);
  }, handleUpload);

  server->onNotFound(notFound); //用户访问未注册的链接时执行notFound()函数
}

//520KB 图片接收需要36秒，速度慢!
//文件上传处理
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  //String logmessage = "Client:" + request->client()->remoteIP().toString() + " " + request->url();
  //Serial.println(logmessage);
  String logmessage ;
  //0表示第一次传输
  if (!index) {
    logmessage = "Upload Start: " + String(filename);
    // open the file on first call and store the file handle in the request object
    handleUpload_start = millis();
    if (SPIFFS.exists(global_bmp))
    {
      Serial.println("delete:" + global_bmp);
      SPIFFS.remove(global_bmp);
    }
    request->_tempFile = SPIFFS.open(global_bmp, "w");
    Serial.println(logmessage);
  }

  if (len) {
    // stream the incoming chunk to the opened file
    request->_tempFile.write(data, len);
    //logmessage = "Writing file: " + String(global_bmp) + " index=" + String(index) + " len=" + String(len);
    //Serial.println(logmessage);
  }

  //最后一次数据传输
  if (final) {
    logmessage = "Upload Complete: " + String(global_bmp) + ",size: " + String(index + len);
    // close the file handle as the upload is now done
    request->_tempFile.close();
    Serial.println(logmessage);
    printf("Upload file took %dms.\n", millis() - handleUpload_start);

    screen_drawBitmap(global_bmp);
    request->redirect("/");
  }
}


void connectwifi()
{
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.disconnect();
  delay(200);

  WiFi.config(IPAddress(192, 168, 1, 200), //设置静态IP位址
              IPAddress(192, 168, 1, 1),
              IPAddress(255, 255, 255, 0),
              IPAddress(192, 168, 1, 1)
             );

  WiFi.mode(WIFI_STA);
  Serial.println("Connecting to WIFI");
  int lasttime = millis() / 1000;
  WiFi.begin(ssid.c_str(), password.c_str());

  while ((!(WiFi.status() == WL_CONNECTED))) {
    delay(1000);
    Serial.print(".");

    //5分钟连接不上，自动重启
    if ( abs(millis() / 1000 - lasttime ) > 300 )
    {
      Serial.println("reboot");
      delay(1000);
      esp_restart();
    }
  }
  Serial.println("Connected");
  Serial.println("My Local IP is : ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);

  // disable Core 0 WDT
  TaskHandle_t idle_0 = xTaskGetIdleTaskHandleForCPU(0);
  esp_task_wdt_delete(idle_0);

  epd_init();
  framebuffer = (uint8_t *)heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 2, MALLOC_CAP_SPIRAM);
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  //初始化spiffs
  if (!SPIFFS.begin(true)) {
    // if you have not used SPIFFS before on a ESP32, it will show this error.
    // after a reboot SPIFFS will be configured and will happily work.
    Serial.println("ERROR: Cannot mount SPIFFS, Rebooting");
    rebootESP();
  }

  Serial.print("SPIFFS Free: "); Serial.println(humanReadableSize((SPIFFS.totalBytes() - SPIFFS.usedBytes())));
  Serial.print("SPIFFS Used: "); Serial.println(humanReadableSize(SPIFFS.usedBytes()));
  Serial.print("SPIFFS Total: "); Serial.println(humanReadableSize(SPIFFS.totalBytes()));

  Serial.println(listFiles());

  Serial.println("\nLoading Configuration ...");

  //配置路由器

  connectwifi();
  if (SPIFFS.exists(global_bmp))
  {
    screen_drawBitmap(global_bmp);
  }

  // startup web server
  Serial.println("Starting Webserver ...");
  //启动网页服务器
  Serial.println("\nConfiguring Webserver ...");
  server = new AsyncWebServer(default_webserverporthttp);
  server->begin();
  configureWebServer();
}


void loop() {
  connectwifi();

  if (rebootESP_flag)
  {
    Serial.println("go to reboot...");
    delay(1000);
    rebootESP();
  }

  if (sleep_flag)
  {
    Serial.println("go to deep sleep...");
    delay(1000);
    epd_poweroff_all();
    esp_sleep_enable_ext1_wakeup(GPIO_SEL_39, ESP_EXT1_WAKEUP_ALL_LOW);
    esp_deep_sleep_start();
  }

  delay(5 * 1000);

}
