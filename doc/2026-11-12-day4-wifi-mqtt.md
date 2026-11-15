# 今天所学总结：WiFi + MQTT（第 4 天）

日期：2026-11-12。目标：让板子把消息发出去，并在网页上收到。第 4 天过关。

配套文件：讲义 `tutorial/lessons/day4-wifi-mqtt.html`；测试程序 `tutorial/mqtt-test/mqtt_test.ino`；Smart Helmet 笔记 `tutorial/reference/smart-helmet-notes.md`；学习记录 `tutorial/learning-records/0007-day4-wifi-mqtt-verified.md`。

---

## 1. 今天之前的状态

推理程序已经在板子上跑通（2026-11-11 验证：静止 idle 1.00、挥动 wave 0.98–1.00、抖动 shake 0.70–1.00，DSP 21 ms、推理 2 ms），但结果只存在串口里。第 4 天的目标是让结果第一次离开板子。

---

## 2. 概念：五个角色各是干什么的

| 东西 | 干什么 | 输入 | 输出 | 跟谁配合 |
|---|---|---|---|---|
| WiFi | 板子连上路由器，才能上网 | WiFi 名字和密码 | 一个 IP 地址 | 路由器 / 手机热点 |
| MQTT（协议） | 小设备用的轻量消息协议，发布 / 订阅模式，基于 TCP | 一条消息 + 一个主题名 | Broker 转发给订阅者 | Broker、PubSubClient |
| Broker（中间人） | 收消息、按主题转发给订阅者 | 各设备发布的消息 | 转发出去的消息 | 发布者、订阅者 |
| 主题 topic | 消息的频道名，发布和订阅的对接点 | — | — | 发布者、订阅者 |
| PubSubClient（库） | Arduino 上实现 MQTT 的库，板子靠它跟 Broker 说话 | 要发的消息 | 发到 Broker 的数据包 | PlatformIO（`lib_deps`）、WiFi |

关键理解：**板子不直接发给网页**，而是发给 Broker；谁想收，就向 Broker 订阅同一个主题。发布和订阅互不认识，只靠主题名对上。

今天用的 Broker 是 HiveMQ 的公共测试服务器 `broker.hivemq.com`，明文、无用户名密码、无 TLS，只发测试消息，不发隐私数据。

### 2.1 三个端口的区别（今天最容易混的地方）

三个端口通向同一个 Broker，消息互通：

- **1883**：MQTT 原生端口，给**设备**走的。板子上的 PubSubClient 说原生 MQTT 协议，走这个门。
- **8000**：WebSocket 端口（`ws://`），给**浏览器**走的。浏览器不能直接说原生 MQTT，只能把 MQTT 包在 WebSocket 里发。
- **8884**：加密的 WebSocket 端口（`wss://`），给浏览器走的，和 8000 同一个作用，只是走了加密通道。

所以代码里写 1883、网页上填 8000（或 8884）都是对的，各走各的门，不用改成一样。

### 2.2 WiFi 的技术限制

- ESP32-S3 只支持 **2.4GHz** 802.11 b/g/n，不支持 5GHz。家里没有 2.4G 网络时，用手机热点：安卓在热点设置里选频段为 2.4GHz；苹果开“最大兼容性”（强制 2.4GHz）。
- 热点名字里的“5G”不一定是频段：比如 `MyPhone 5G` 里的 5G 是手机型号（5G 手机），频段要看热点设置里的单独选项。
- 换热点频段（5G → 2.4G）等于把热点重启一次：电脑会掉线几秒到十几秒，然后用同一个名字和密码自动连回来，只是速度降到 2.4G 水平。
- 板子和电脑**不用在同一个局域网**：消息经过公共 Broker 中转，板子走热点、电脑走别的能上网的网络就行。

### 2.3 MQTT 的补充细节

- QoS：今天用的默认 QoS 0（发一次，不管对方收到没有）。订阅时网页显示 QoS 2 只是订阅端请求的等级，实际按发布端 QoS 0 投递。
- Keep Alive：网页客户端默认 60 秒；板子侧 `client.loop()` 必须经常被调用，用来维持心跳和收发。
- ClientID 必须唯一：测试程序用芯片 EFuse MAC 拼了一个 `esp32s3-aio-<十六进制>`，避免和别人撞名。
- PubSubClient 默认最大包 256 字节，短消息没问题；长 JSON 要注意这个上限。
- 公共 Broker 是共享的：主题是公开的，谁订阅都能收到，所以别发隐私，WiFi 密码也别提交进仓库。

---

## 3. 测试程序是怎么写的

文件：`tutorial/mqtt-test/mqtt_test.ino`（临时程序，只测联网，不读 MPU6050、不跑模型，这样出了问题好定位）。

### 3.1 需要改的配置

```ini
# platformio.ini 里加（已有 lib_deps 就追加这一行）
lib_deps =
knolleary/PubSubClient@^2.8
```

```cpp
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

const char *mqtt_server = "broker.hivemq.com";
const uint16_t mqtt_port = 1883;
const char *mqtt_topic = "esp32s3-tinyml/aio/test";
```

WiFi 名字和密码在自己电脑上改，不用发给 Muse。

### 3.2 三个函数各是干什么的

- `setup_wifi()`：输入是 `WIFI_SSID` / `WIFI_PASSWORD`，输出是连上 WiFi 并打印 IP。`WiFi.begin()` 后最多等 20 秒（`millis()` 计时，每 500 ms 打印一个 `.`），超时就打印失败，不死等。
- `setup()`：输入无，输出是完成初始化。串口 115200、调 `setup_wifi()`、`client.setServer(mqtt_server, mqtt_port)` 告诉 PubSubClient 去连哪个 Broker。
- `loop()`：每轮做三件事——① WiFi 掉了就重连；② MQTT 没连上就用 `client.connect(clientId)` 连，失败打印 `rc=`（`client.state()` 的返回值）并等 5 秒重试；③ 每 5 秒用 `snprintf` 拼一条 `hello from ESP32-S3 #N`，`client.publish(mqtt_topic, payload)` 发出去，成功打印“已发布到 …”，失败打印“发布失败”。`client.loop()` 每轮都要调，维持 MQTT 心跳。

### 3.3 串口应该看到什么

```
正在连接 WiFi：<你的热点名>
............
WiFi 已连接，IP 地址：
192.168.x.x
正在连接 MQTT Broker broker.hivemq.com:1883... MQTT 已连接
已发布到 esp32s3-tinyml/aio/test：hello from ESP32-S3 #1
已发布到 esp32s3-tinyml/aio/test：hello from ESP32-S3 #2
……
```

### 3.4 网页上怎么验证收到

打开 `https://www.hivemq.com/demos/websocket-client/`，点 Connect（默认 Host `broker.hivemq.com`、Port 8000，用户名密码空着，SSL 不勾），红点变绿表示连上；再在 Subscriptions 里订阅主题 `esp32s3-tinyml/aio/test`，Messages 区每 5 秒出现一条新消息。

---

## 4. 今天遇到的问题和排查

### 4.1 hivemq.com 网页打不开（ERR_TIMED_OUT）

现象：第一次打开 `hivemq.com/demos/websocket-client` 显示“无法访问此网站，响应时间过长”。
原因：手机热点网络到这个网站慢，不是地址错了。
解决：刷新重进一次就打开了。以后遇到先刷新，别先改地址。

### 4.2 网页客户端连不上 Broker（AMQJSC0001E Connect timed out）

现象：点 Connect 后弹窗 `Connect failed: AMQJSC0001E Connect timed out`，红点不变绿。
原因：电脑上开了梯子（VPN），梯子拦住了 8000 端口的 WebSocket 连接（或它那个节点到 HiveMQ 的连接太慢）。
证据：板子一直正常（串口发到 #26），因为板子的流量直连手机热点，根本不经过电脑上的梯子；只有浏览器的流量经过梯子。
解决：勾选 SSL，把 Port 从 8000 改成 **8884** 再点 Connect，成功连上。经验：以后连 HiveMQ 网页客户端，要么关掉梯子，要么走 8884 加密端口。

### 4.3 代码里 1883、网页上 8000，对不上是不是错了

不是错了，见 2.1：1883 是设备的原生 MQTT 口，8000/8884 是浏览器的 WebSocket 口，通向同一个 Broker，消息互通，各走各的门。

### 4.4 换热点频段会不会影响正在连热点的电脑

会，但只是短暂断开：换频段等于热点重启，电脑掉线几秒到十几秒后自动连回来（名字密码不变），速度降到 2.4G 水平。验证 MQTT 对网速没要求，直接换就行。

---

## 5. 验证结果（第 4 天过关）

- 板子侧：串口显示 WiFi 已连接、MQTT 已连接，每 5 秒一条“已发布”，编号一直涨到 #26 及之后。
- 网页侧：HiveMQ 网页客户端显示 `connected`，订阅 `esp32s3-tinyml/aio/test` 后收到 `#114`、`#115`、`#116`、`#117`（2026-11-12 14:44:40–14:45:05，每 5 秒一条）。
- 结论：板子 → Broker → 网页整条路通了，第 4 天过关。清单 Day 4 已勾选。

---

## 6. 不能直接抄的东西（给第 5 天留的坑）

Smart Helmet 的重连写法是阻塞式的：WiFi 连不上就 `while` 死等，MQTT 断线就 `delay(5000)` 再试。测试程序这样写没关系（不采样），但**不能直接搬进推理程序**，因为推理程序每 20 毫秒必须读一次传感器，`delay(5000)` 会把采样节拍打断。第 5 天整合时要改成非阻塞的重连写法（或留到 Day 11 用 FreeRTOS 任务拆开）。

其他注意：

- Smart Helmet 的看板是坏的（`app.js` 缺失、默认 Broker 是 EMQX，和固件的 HiveMQ 对不上），不能直接拿来用；第 6 天看板要自己写。
- 推理程序的原稿保存在 `tutorial/deployment/main.cpp`（工作区里的文件，不是电脑本地 `G:\ESP32_PRJ` 里的）。做完第 4 天要把 `src/main.ino` 恢复成推理程序，第 5 天直接在这个程序上改。
- 工作区和电脑本地是两个地方：Muse 写的文件存在工作区（App 里能看到），电脑本地 `G:\ESP32_PRJ` 里的文件要由 aio 自己复制进去。

---

## 7. 面试能讲的四句话（第 4 天版）

- **输入：** 家里 WiFi（或手机热点 2.4GHz）的名字和密码、HiveMQ 公共 Broker 的地址和端口。
- **输出：** 板子每 5 秒向主题 `esp32s3-tinyml/aio/test` 发一条测试消息，网页客户端能收到。
- **改了哪里：** 没动推理程序；新加的是 `PubSubClient` 库和一个只测联网的临时程序，推理程序原稿另存着。
- **为什么改：** 推理结果要离开板子、能被网页和看板收到，得先证明“板子 → Broker → 网页”这条路是通的，再往里面塞真正的数据。

---

## 8. 回忆自测（标准答案版）

1. **MQTT 里，板子和网页是怎么对上同一条消息的？中间人是谁？**
标准答案：靠同一个主题名对上。板子向主题 `esp32s3-tinyml/aio/test` 发布，网页向同一个主题订阅；中间人是 Broker（`broker.hivemq.com`），它收发布者的消息、转发给订阅者。发布和订阅互不认识。
2. **为什么今天要先用一个不读传感器的测试程序，而不是直接改推理程序？**
标准答案：隔离变量。只测联网，出了问题只可能是 WiFi、MQTT、Broker、网络这几处；如果直接改推理程序，串口没消息时分不清是传感器、模型还是网络的问题。
3. **Smart Helmet 的断线重连写法为什么不能直接搬进推理程序？**
标准答案：它是阻塞式的，`delay(5000)` 会卡住 5 秒，而推理程序每 20 毫秒必须读一次传感器。卡住就会丢采样、窗口错位，推理结果就错了。第 5 天要改成非阻塞写法。
4. **1883、8000、8884 三个端口分别给谁用？**
标准答案：1883 给设备走原生 MQTT；8000 给浏览器走 WebSocket（`ws://`）；8884 给浏览器走加密 WebSocket（`wss://`）。三个门通向同一个 Broker，消息互通。
5. **网页客户端 8000 端口超时，板子却正常，说明问题出在哪？**
标准答案：问题在电脑到 Broker 的这条路上，不在板子。板子直连热点、走 1883，正常；浏览器经过电脑上的梯子、走 8000，被拦住了。证据就是板子串口一直在发，网页连不上。
