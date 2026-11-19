/*
 * ESP32-S3 手势识别固件。
 * 传感器任务执行 MPU6050 采样、TinyML 推理与 RGB 状态反馈。
 * 主循环通过队列接收结果，执行 MQTT 上报、持续抖动告警与 ArduinoOTA 服务。
 * Wi-Fi 连接后启用 modem sleep。
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <MPU6050.h> // Jeff Rowberg MPU6050 驱动，依赖 I2Cdev
#include <ESP32-S3-TinyML_inferencing.h> // Edge Impulse 模型推理库

/* 固件版本标识。 */
#define FW_VERSION "12.1"

/* 本地 Wi-Fi 凭据，文件由 Git 忽略。 */
#include "secrets.h"

/* MQTT Broker 与发布主题。 */
const char *mqtt_server = "broker.hivemq.com";
const uint16_t mqtt_port = 1883;
const char *mqtt_topic = "esp32s3-tinyml/aio/gesture";
const char *mqtt_alert_topic = "esp32s3-tinyml/aio/alert";

/* 持续抖动告警阈值。 */
#define ALERT_SHAKE_MIN_CONF 0.80f /* shake 类别的置信度下限 */
#define ALERT_CONSECUTIVE 3        /* 连续达标窗口数，约对应 6 秒 */
static int shake_streak = 0;       /* 连续达标计数，仅由主循环更新 */
static bool alert_active = false;  /* 当前连续事件的告警触发标记 */

WiFiClient espClient;
PubSubClient mqtt(espClient);
unsigned long last_mqtt_try_ms = 0; /* 最近一次 MQTT 连接尝试时间 */

/* 任务间传递的分类结果。 */
struct GestureResult {
  char label[8];    /* "idle" / "shake" / "wave" */
  float confidence; /* 最高类别置信度 */
  float p_idle;
  float p_shake;
  float p_wave;
};
static QueueHandle_t result_queue = nullptr; /* 传感器任务向主循环传递结果 */
#define RESULT_QUEUE_LEN 4                   /* 队列满时丢弃新结果，发送操作不等待 */

/* MPU6050 I2C 引脚。 */
#define MPU6050_SDA 8
#define MPU6050_SCL 9

/* 板载 WS2812 RGB 灯。 */
#define RGB_LED_PIN 48
#define LED_BRIGHT 32 // RGB 亮度值

/* 类别与灯色映射，由传感器任务调用。 */
void set_led_by_label(const char *label) {
  if (strcmp(label, "idle") == 0) {
    neopixelWrite(RGB_LED_PIN, 0, LED_BRIGHT, 0); // 绿色
  } else if (strcmp(label, "wave") == 0) {
    neopixelWrite(RGB_LED_PIN, 0, 0, LED_BRIGHT); // 蓝色
  } else if (strcmp(label, "shake") == 0) {
    neopixelWrite(RGB_LED_PIN, LED_BRIGHT, 0, 0); // 红色
  }
}

/* MPU6050 校准偏移量。 */
#define ACCEL_X_OFFSET -160
#define ACCEL_Y_OFFSET 918
#define ACCEL_Z_OFFSET 1639
#define GYRO_X_OFFSET 61
#define GYRO_Y_OFFSET 45
#define GYRO_Z_OFFSET 9

/* ±4g 量程下的加速度单位换算系数。 */
#define ACCEL_SENSITIVITY 8192.0f
#define G_TO_MS2 9.80665f

MPU6050 mpu;

/* 单窗口加速度缓冲区，仅由传感器任务访问。 */
static float inference_buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
static size_t buffer_ix = 0;

/* Edge Impulse 输入数据读取回调。 */
int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
  memcpy(out_ptr, inference_buffer + offset, length * sizeof(float));
  return 0;
}

/* 启动时连接 Wi-Fi，等待时间上限为 20 秒。 */
void connect_wifi() {
  Serial.print("正在连接 WiFi：");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi 已连接，IP 地址：");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi 连接失败，推理照常运行，只是不上报，OTA 也用不了");
  }
}

/* 主循环维护 MQTT 连接，按 5 秒间隔尝试重连。 */
void maintain_mqtt() {
  if (WiFi.status() != WL_CONNECTED) return; // Wi-Fi 未连接时跳过 MQTT 维护
  if (mqtt.connected()) {
    mqtt.loop(); // 处理 MQTT 心跳
    return;
  }
  unsigned long now = millis();
  if (now - last_mqtt_try_ms < 5000) return; // 限制连接尝试频率
  last_mqtt_try_ms = now;

  /* 使用芯片标识生成 MQTT client ID。 */
  String clientId = "esp32s3-aio-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(clientId.c_str())) {
    Serial.println("MQTT 已连接");
  } else {
    Serial.print("MQTT 连接失败 rc=");
    Serial.print(mqtt.state());
    Serial.println("，5 秒后再试（推理不受影响）");
  }
}

/* Wi-Fi 连接后初始化 OTA，由主循环处理上传请求。 */
void setup_ota() {
  ArduinoOTA.setHostname("esp32s3-aio");
  ArduinoOTA.onStart([]() {
    Serial.println("OTA 开始接收新固件……（这段时间推理会暂停）");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA 接收完成，正在重启进新固件");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int last_pct = 101;
    unsigned int pct = progress * 100 / total;
    if (pct / 10 != last_pct / 10) { // 以 10% 为步长输出上传进度
      last_pct = pct;
      Serial.printf("OTA 进度：%u%%\n", pct);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA 出错 (%u)，旧固件不受影响，重启后还是旧版本\n", error);
  });
  ArduinoOTA.begin();
  Serial.println("OTA 已启动：PlatformIO 选 esp32s3_ota 环境即可无线上传");
}

/* 模型推理、灯色反馈与结果入队。 */
void classify_and_send() {
  signal_t signal;
  signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
  signal.get_data = &raw_feature_get_data;

  ei_impulse_result_t result = {0};
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
  if (err != EI_IMPULSE_OK) {
    ei_printf("推理失败 (%d)\n", err);
    return;
  }

  /* 选择置信度最高的类别。 */
  uint8_t best_ix = 0;
  for (uint8_t i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > result.classification[best_ix].value) {
      best_ix = i;
    }
  }
  const char *best_label = ei_classifier_inferencing_categories[best_ix];

  /* 更新分类状态灯。 */
  set_led_by_label(best_label);

  ei_printf("预测：%s (%.2f) | idle %.2f shake %.2f wave %.2f [DSP %d ms, 推理 %d ms]\n",
            best_label,
            result.classification[best_ix].value,
            result.classification[0].value,
            result.classification[1].value,
            result.classification[2].value,
            result.timing.dsp,
            result.timing.classification);

  /* 非阻塞发送分类结果；队列满时丢弃当前结果。 */
  GestureResult r;
  strncpy(r.label, best_label, sizeof(r.label) - 1);
  r.label[sizeof(r.label) - 1] = '\0';
  r.confidence = result.classification[best_ix].value;
  r.p_idle = result.classification[0].value;
  r.p_shake = result.classification[1].value;
  r.p_wave = result.classification[2].value;
  if (xQueueSend(result_queue, &r, 0) != pdTRUE) {
    ei_printf("（结果队列已满，本次结果丢弃：网络端长时间没取走）\n");
  }
}

void sensor_task(void *pvParameters) {
  TickType_t last_wake = xTaskGetTickCount();
  for (;;) {
    /* 按 20 ms 周期调度采样；同步推理可能造成周期超限。 */
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(20));

    int16_t ax_raw, ay_raw, az_raw;
    mpu.getAcceleration(&ax_raw, &ay_raw, &az_raw);

    /* 将传感器原始计数转换为 m/s²。 */
    inference_buffer[buffer_ix++] = ax_raw / ACCEL_SENSITIVITY * G_TO_MS2;
    inference_buffer[buffer_ix++] = ay_raw / ACCEL_SENSITIVITY * G_TO_MS2;
    inference_buffer[buffer_ix++] = az_raw / ACCEL_SENSITIVITY * G_TO_MS2;

    /* 累积一个完整输入窗口后执行推理。 */
    if (buffer_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
      buffer_ix = 0;
      classify_and_send();
    }
  }
}

/* 主循环处理分类上报与告警状态。 */
void handle_result(const GestureResult &r) {
  /* MQTT 连接有效时发布分类结果。 */
  if (mqtt.connected()) {
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"label\":\"%s\",\"confidence\":%.2f,\"idle\":%.2f,\"shake\":%.2f,\"wave\":%.2f}",
             r.label, r.confidence, r.p_idle, r.p_shake, r.p_wave);
    if (mqtt.publish(mqtt_topic, payload)) {
      ei_printf("已上报：%s\n", payload);
    } else {
      ei_printf("上报失败（包太大或连接刚断开）\n");
    }
  } else {
    ei_printf("（MQTT 未连接，本次结果不上报）\n");
  }

  /* 根据连续达标窗口触发持续抖动告警。 */
  if (strcmp(r.label, "shake") == 0 && r.p_shake >= ALERT_SHAKE_MIN_CONF) {
    shake_streak++;
    if (shake_streak >= ALERT_CONSECUTIVE && !alert_active) {
      alert_active = true; /* 同一连续事件仅触发一次，未达标窗口解除锁存 */
      if (mqtt.connected()) {
        char alert_payload[128];
        snprintf(alert_payload, sizeof(alert_payload),
                 "{\"type\":\"alert\",\"event\":\"shake\",\"confidence\":%.2f,\"count\":%d}",
                 r.p_shake, shake_streak);
        if (mqtt.publish(mqtt_alert_topic, alert_payload)) {
          ei_printf("告警已上报：%s\n", alert_payload);
        } else {
          ei_printf("告警触发，但发布失败（包太大或连接刚断开）\n");
        }
      } else {
        ei_printf("告警触发，但 MQTT 未连接，本次告警未上报\n");
      }
    }
  } else {
    /* 未达标窗口重置计数与告警锁存状态。 */
    shake_streak = 0;
    alert_active = false;
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { /* 串口初始化等待上限为 3 秒 */ }

  Serial.printf("ESP32-S3 手势识别（低功耗 + 准确率复测），固件版本：%s\n", FW_VERSION);
  Wire.begin(MPU6050_SDA, MPU6050_SCL);
  Wire.setClock(400000);

  mpu.initialize();
  if (!mpu.testConnection()) {
    Serial.println("MPU6050 连接失败！检查接线（SDA=GPIO8, SCL=GPIO9）");
    while (1) { delay(1000); }
  }

  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_4); // ±4g，与模型输入量程一致
  mpu.setXAccelOffset(ACCEL_X_OFFSET);
  mpu.setYAccelOffset(ACCEL_Y_OFFSET);
  mpu.setZAccelOffset(ACCEL_Z_OFFSET);
  mpu.setXGyroOffset(GYRO_X_OFFSET);
  mpu.setYGyroOffset(GYRO_Y_OFFSET);
  mpu.setZGyroOffset(GYRO_Z_OFFSET);

  /* 启动状态显示暗蓝色，首次推理后切换为分类灯色。 */
  neopixelWrite(RGB_LED_PIN, 0, 0, 16);

  connect_wifi();
  mqtt.setServer(mqtt_server, mqtt_port);

  /* Wi-Fi 低功耗配置。 */
  if (WiFi.status() == WL_CONNECTED) {
    /* 输出配置前的 modem sleep 状态与当前 CPU 频率。 */
    Serial.printf("低功耗现状：WiFi modem sleep %s，CPU %d MHz\n",
                  WiFi.getSleep() ? "已开" : "未开", getCpuFrequencyMhz());
    WiFi.setSleep(true); /* 空闲时降低 Wi-Fi 射频活动，保持网络连接 */
    Serial.printf("已显式打开 modem sleep，现在是：%s\n", WiFi.getSleep() ? "已开" : "未开");
  }

  if (WiFi.status() == WL_CONNECTED) {
    setup_ota();
  }

  /* 创建结果队列及传感器任务，任务栈为 16384 字节、优先级为 2、固定在核心 1。 */
  result_queue = xQueueCreate(RESULT_QUEUE_LEN, sizeof(GestureResult));
  if (result_queue == nullptr) {
    Serial.println("队列创建失败！");
    while (1) { delay(1000); }
  }
  if (xTaskCreatePinnedToCore(sensor_task, "sensor", 16384, nullptr, 2, nullptr, 1) != pdPASS) {
    Serial.println("传感器任务创建失败！");
    while (1) { delay(1000); }
  }

  Serial.println("MPU6050 已连接，双任务已启动（每 2 秒出一次结果）");
  Serial.println("先静止 2 秒：应输出 idle，置信度接近 1");
}

void loop() {
  ArduinoOTA.handle();  /* 处理 OTA 请求 */
  maintain_mqtt();      /* MQTT 维护与传感器采样分别运行于独立任务 */

  /* 等待分类结果最多 10 ms，随后继续处理 OTA 与 MQTT。 */
  GestureResult r;
  if (xQueueReceive(result_queue, &r, pdMS_TO_TICKS(10)) == pdTRUE) {
    handle_result(r);
  }
}
