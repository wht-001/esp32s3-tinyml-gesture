/*
* Day 5 整合程序：手势识别推理 + MQTT 上报
* ============================================================
* 这是什么：
* 在第 7 课推理程序（deployment/main.cpp）的基础上，加一层联网：
* 开机连 WiFi，每次推理出结果，就把标签和置信度发一条 JSON 到 MQTT。
* 推理逻辑（采样、换算、窗口、分类、灯反馈）一个字没改。
*
* 新增的三块：
* 1. connect_wifi() 开机连热点，带 20 秒超时；连不上也继续跑，
* 推理照常，只是不上报。
* 2. maintain_mqtt() 非阻塞地维持 MQTT 连接：断了就每 5 秒试连一次，
* 试不通就跳过。为什么不用 delay 死等：
* 主循环每 20 毫秒必须读一次传感器，
* delay(5000) 会丢 250 个采样点，窗口就错位了。
* 3. 推理后发布 连得上才发，连不上就跳过，不影响推理。
*
* 发到哪：
* Broker：broker.hivemq.com:1883（HiveMQ 公共测试服务器）
* 主题： esp32s3-tinyml/aio/gesture
* 格式： {"label":"wave","confidence":0.98,"idle":0.00,"shake":0.02,"wave":0.98}
* 约 90 字节，在 PubSubClient 默认 256 字节上限内。
*
* 验证：HiveMQ 网页客户端订阅上面的主题（关掉梯子，或走 SSL 8884 端口），
* 静止收到 idle，挥动收到 wave，抖动收到 shake。
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <MPU6050.h> // Jeff Rowberg MPU6050 库（经 I2Cdev）
#include <ESP32-S3-TinyML_inferencing.h> // 导出的模型库

/* ---------- 本机 WiFi 配置（复制 include/secrets.example.h 为 secrets.h 后填写） ---------- */
#include "secrets.h"

/* ---------- MQTT 设置 ---------- */
const char *mqtt_server = "broker.hivemq.com";
const uint16_t mqtt_port = 1883;
const char *mqtt_topic = "esp32s3-tinyml/aio/gesture";

WiFiClient espClient;
PubSubClient mqtt(espClient);
unsigned long last_mqtt_try_ms = 0; /* 上次试连 MQTT 的时间 */

/* ---------- 硬件接线（和第 4 课采集程序一致） ---------- */
#define MPU6050_SDA 8
#define MPU6050_SCL 9

/* ---------- 板载 RGB 灯：WS2812，GPIO48 ---------- */
#define RGB_LED_PIN 48
#define LED_BRIGHT 32 // WS2812 很亮，32 就够了

/* 按推理结果换灯色：idle 绿 / wave 蓝 / shake 红 */
void set_led_by_label(const char *label) {
if (strcmp(label, "idle") == 0) {
neopixelWrite(RGB_LED_PIN, 0, LED_BRIGHT, 0); // 绿
} else if (strcmp(label, "wave") == 0) {
neopixelWrite(RGB_LED_PIN, 0, 0, LED_BRIGHT); // 蓝
} else if (strcmp(label, "shake") == 0) {
neopixelWrite(RGB_LED_PIN, LED_BRIGHT, 0, 0); // 红
}
}

/* ---------- 第 4 课的校准偏移 ---------- */
#define ACCEL_X_OFFSET -160
#define ACCEL_Y_OFFSET 918
#define ACCEL_Z_OFFSET 1639
#define GYRO_X_OFFSET 61
#define GYRO_Y_OFFSET 45
#define GYRO_Z_OFFSET 9

/* ---------- 单位换算：±4G 量程，8192 LSB/g ---------- */
#define ACCEL_SENSITIVITY 8192.0f
#define G_TO_MS2 9.80665f

MPU6050 mpu;

/* 攒数据的缓冲区：99 采样 × 3 轴 = 297 个 float */
static float inference_buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
static size_t buffer_ix = 0;
static unsigned long last_sample_ms = 0;

/* 模型库从这个回调里取数据 */
int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
memcpy(out_ptr, inference_buffer + offset, length * sizeof(float));
return 0;
}

void run_inference();

/* 连 WiFi，最多等 20 秒；连不上不阻塞开机流程后面的推理 */
void connect_wifi() {
Serial.print("正在连接 WiFi：");
Serial.println(WIFI_SSID);
WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

unsigned long t0 = millis();
while (WiFi.status()!= WL_CONNECTED && millis() - t0 < 20000) {
delay(500);
Serial.print(".");
}

if (WiFi.status() == WL_CONNECTED) {
Serial.println("\nWiFi 已连接，IP 地址：");
Serial.println(WiFi.localIP());
} else {
Serial.println("\nWiFi 连接失败，推理照常运行，只是不上报");
}
}

/* 非阻塞地维持 MQTT 连接：连不上就跳过，采样优先 */
void maintain_mqtt() {
if (WiFi.status()!= WL_CONNECTED) return; // WiFi 没连上，先不管
if (mqtt.connected()) {
mqtt.loop(); // 维持心跳
return;
}
unsigned long now = millis();
if (now - last_mqtt_try_ms < 5000) return; // 5 秒内试过，先跳过
last_mqtt_try_ms = now;

/* clientId 加一段芯片编号，避免和别人撞名 */
String clientId = "esp32s3-aio-" + String((uint32_t)ESP.getEfuseMac(), HEX);
if (mqtt.connect(clientId.c_str())) {
Serial.println("MQTT 已连接");
} else {
Serial.print("MQTT 连接失败 rc=");
Serial.print(mqtt.state());
Serial.println("，5 秒后再试（推理不受影响）");
}
}

void setup() {
Serial.begin(115200);
unsigned long t0 = millis();
while (!Serial && millis() - t0 < 3000) { /* 最多等 3 秒 */}

Serial.println("ESP32-S3 手势识别推理 + MQTT 上报");
Wire.begin(MPU6050_SDA, MPU6050_SCL);
Wire.setClock(400000);

mpu.initialize();
if (!mpu.testConnection()) {
Serial.println("MPU6050 连接失败！检查接线（SDA=GPIO8, SCL=GPIO9）");
while (1) { delay(1000);}
}

mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_4); // ±4G，和训练一致
mpu.setXAccelOffset(ACCEL_X_OFFSET);
mpu.setYAccelOffset(ACCEL_Y_OFFSET);
mpu.setZAccelOffset(ACCEL_Z_OFFSET);
mpu.setXGyroOffset(GYRO_X_OFFSET);
mpu.setYGyroOffset(GYRO_Y_OFFSET);
mpu.setZGyroOffset(GYRO_Z_OFFSET);

/* 先亮一个暗蓝色，表示程序已启动；第一次推理后按结果换色 */
neopixelWrite(RGB_LED_PIN, 0, 0, 16);

connect_wifi();
mqtt.setServer(mqtt_server, mqtt_port);

Serial.println("MPU6050 已连接，开始推理（每 2 秒出一次结果）");
Serial.println("先静止 2 秒：应输出 idle，置信度接近 1");
}

void loop() {
maintain_mqtt(); /* 非阻塞，连不上不卡采样 */

/* 50Hz：每 20 毫秒读一次 */
unsigned long now = millis();
if (now - last_sample_ms < 20) return;
last_sample_ms = now;

int16_t ax_raw, ay_raw, az_raw;
mpu.getAcceleration(&ax_raw, &ay_raw, &az_raw);

/* 原始计数 → m/s²，和采集程序的换算一致 */
inference_buffer[buffer_ix++] = ax_raw / ACCEL_SENSITIVITY * G_TO_MS2;
inference_buffer[buffer_ix++] = ay_raw / ACCEL_SENSITIVITY * G_TO_MS2;
inference_buffer[buffer_ix++] = az_raw / ACCEL_SENSITIVITY * G_TO_MS2;

/* 攒满一个窗口（99 个采样，约 2 秒）就跑一次推理 */
if (buffer_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
buffer_ix = 0;
run_inference();
}
}

void run_inference() {
signal_t signal;
signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
signal.get_data = &raw_feature_get_data;

ei_impulse_result_t result = { 0};
EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
if (err!= EI_IMPULSE_OK) {
ei_printf("推理失败 (%d)\n", err);
return;
}

/* 找置信度最高的标签 */
uint8_t best_ix = 0;
for (uint8_t i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
if (result.classification[i].value > result.classification[best_ix].value) {
best_ix = i;
}
}
const char *best_label = ei_classifier_inferencing_categories[best_ix];

/* 灯色跟着结果走 */
set_led_by_label(best_label);

ei_printf("预测：%s (%.2f) | idle %.2f shake %.2f wave %.2f [DSP %d ms, 推理 %d ms]\n",
best_label,
result.classification[best_ix].value,
result.classification[0].value,
result.classification[1].value,
result.classification[2].value,
result.timing.dsp,
result.timing.classification);

/* 把结果发到 MQTT（连不上就跳过，不影响推理） */
if (mqtt.connected()) {
char payload[128];
snprintf(payload, sizeof(payload),
"{\"label\":\"%s\",\"confidence\":%.2f,\"idle\":%.2f,\"shake\":%.2f,\"wave\":%.2f}",
best_label,
result.classification[best_ix].value,
result.classification[0].value,
result.classification[1].value,
result.classification[2].value);
if (mqtt.publish(mqtt_topic, payload)) {
ei_printf("已上报：%s\n", payload);
} else {
ei_printf("上报失败（包太大或连接刚断开）\n");
}
} else {
ei_printf("（MQTT 未连接，本次结果不上报）\n");
}
}
