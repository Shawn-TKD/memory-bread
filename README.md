# 记忆面包（Memory Bread）

一个可以装进口袋的开源墨水屏 AI 语音笔记设备。

按住录音键说话，松开即保存；也可以连续短按三次进入长录音。设备优先把完整 WAV 保存在 microSD 卡，等回到家或办公室后，再手动连接 Wi-Fi，使用硅基流动完成中文语音转写和 AI 整理，并通过闪念贝壳 MCP 保存成可检索的文字笔记。

当前稳定固件：**v1.5-memorybread**

> 本项目仍处于实机验证阶段。录音默认离线保存，云端转写和同步均由用户主动触发。

## 它能做什么

- 长按录音键记录碎片想法，松开后保存。
- 亮屏时连续短按录音键三次，开始一个连续长 WAV；再短按一次结束。
- 16 kHz、16 位、单声道 WAV，边采集边写入 microSD，不按分钟拆段。
- 200 × 200 黑白墨水屏中文 UI，内置全部 20,992 个 Unicode 基本区汉字。
- 最多保存三组 2.4 GHz Wi-Fi，支持热点配网和附近网络扫描。
- 使用硅基流动 `FunAudioLLM/SenseVoiceSmall` 完成 ASR。
- 使用硅基流动 `deepseek-ai/DeepSeek-V3.2` 生成标题、摘要和整理正文。
- 通过闪念贝壳 MCP `note_create` 创建文字笔记，并用本地标记避免重复上传。
- 手机网页端浏览、播放、下载、删除笔记和管理标签。
- 三种提示音主题、深度睡眠、低电量保护和 HTTPS OTA。
- 录音离线可用；没有 Wi-Fi、API Key 或 MCP Token 时，仍可作为普通录音设备。

## 硬件

固件针对以下开发板：

**Waveshare ESP32-S3-ePaper-1.54（黑白屏、N8R8）**

- ESP32-S3 双核 240 MHz
- 8 MB Flash + 8 MB OPI PSRAM
- 1.54 英寸 200 × 200 黑白电子墨水屏
- 板载音频编解码、麦克风和扬声器
- microSD / TF 卡槽
- RTC、SHTC3 温湿度传感器
- 锂电池充放电管理
- BOOT / GPIO0 录音键与 PWR / GPIO18 电源键
- 2.4 GHz Wi-Fi 与 Bluetooth LE

参考购买页面：[微雪 ESP32-S3-ePaper-1.54](https://www.waveshare.net/shop/ESP32-S3-ePaper-1.54-EN.htm)

本版固件不使用触摸功能。四彩 `1.54G` 版本和 RP2350 版本不是当前固件的目标板型。

## 工作流程

```text
按键录音
   ↓
microSD 保存完整 WAV
   ↓  用户手动进入“同步”
硅基流动 SenseVoiceSmall 语音转写
   ↓
microSD 保存 TXT
   ↓
DeepSeek-V3.2 整理标题、摘要和正文
   ↓
闪念贝壳 MCP note_create
```

原始 WAV 不会上传给闪念贝壳。同步失败不会删除本地 WAV 或 TXT，下次可以继续重试。

## 快速上手

### 1. 准备材料

- 同款微雪 ESP32-S3 墨水屏开发板
- FAT32 格式 microSD 卡
- 支持数据传输的 USB-C 线
- 2.4 GHz Wi-Fi
- 硅基流动 API Key（云端转写需要）
- 闪念贝壳 MCP Token（同步到闪念贝壳需要）

### 2. 烧录固件

安装 Arduino CLI、ESP32 3.2.0 核心及依赖：

```bash
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.2.0
arduino-cli lib install "Adafruit GFX Library" "ArduinoJson"
```

在仓库根目录编译：

```bash
arduino-cli compile \
  -b "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=custom,CDCOnBoot=cdc,FlashSize=8M" \
  ./forrest_note
```

进入 ROM Bootloader：

1. 拔下 USB；如果电池仍在供电，先断开电池。
2. 按住 BOOT / GPIO0 录音键。
3. 保持按住并插入 USB 数据线。
4. 等待约两秒后松开 BOOT。
5. 用 `arduino-cli board list` 找到串口。

烧录：

```bash
arduino-cli compile --upload \
  -p /dev/tty.usbmodemXXXX \
  -b "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=custom,CDCOnBoot=cdc,FlashSize=8M" \
  ./forrest_note
```

日志出现 `Hash of data verified.` 代表写入校验成功。普通烧录不会清除 NVS 配置和 microSD 卡内容。

### 3. 配置 Wi-Fi 和服务

1. 在设备打开“设置 → 传输”。
2. 如果还没有可用 Wi-Fi，连接设备热点 `MemoryBread-Setup`。
3. 浏览器访问 `http://192.168.4.1/provision`。
4. 选择附近的 2.4 GHz Wi-Fi，最多可以保存三组网络。
5. 填写硅基流动 API Key。
6. 填写闪念贝壳 Bearer Token，并勾选自动写入开关。
7. 保存设置，联网后点击“测试连接”。

Key 和 Token 通过配置页存入 ESP32 的 NVS，不需要写进源码。完整步骤见[中文使用说明书](./USER_GUIDE_ZH.md)。

## 按键操作

| 场景 | 电源键 PWR | 录音键 BOOT / GPIO0 |
|---|---|---|
| 睡眠 | 唤醒进入菜单 | 按住唤醒并开始碎片录音 |
| 待机 | 进入菜单 | 长按碎片录音；三击进入长录音 |
| 菜单 | 短按移动 | 短按确认；长按返回 |
| 长录音 | — | 短按结束并保存 |
| 笔记列表 | 选择下一条 | 短按打开；长按返回 |
| 笔记详情 | 短按翻页；长按删除 | 短按播放/停止；长按返回 |

录音关闭成功后会立即写入笔记索引，再进入标签选择页面。v1.5 还会在开机时恢复 microSD 卡中“已有 WAV、但索引缺失”的有效录音，并避免新录音覆盖这些文件。

## 长录音

- 亮屏待机时三击录音键，或者进入“菜单 → 长录音”。
- 再短按录音键结束。
- 一次录音生成一个连续 WAV，不会每分钟拆分。
- 每约 30 秒更新一次 WAV 文件头，降低异常断电后整段无法播放的风险。
- 空间不足、电量约 5% 或达到 12 小时安全上限时自动结束并保存。
- 音频约占 1.92 MB/分钟，即约 115 MB/小时。

硅基流动音频转写接口当前限制单个文件不超过 50 MB、时长不超过一小时。按本设备格式，超过约 25 分钟的录音会继续完整保存在 microSD，但当前固件尚未实现云端转写分块。

闪念贝壳整理流程当前最多读取转写文本前 20,000 字节，送入 DeepSeek 整理的内容为前 6,000 字节。因此很长的会议录音暂时不适合直接生成完整会议总结；后续需要加入文本分段总结与合并。

## 存储和分区

8 MB Flash 当前使用双 OTA 分区：

| 分区 | 大小 | 用途 |
|---|---:|---|
| App 0 | 3 MB | 当前运行固件 |
| App 1 | 3 MB | OTA 备用固件 |
| SPIFFS | 约 1.875 MB | 内部数据区 |
| 其他 | 约 0.125 MB | NVS、OTA 信息和崩溃记录 |

当前开源构建的 v1.5 应用 BIN 为 2,193,568 字节，单个 App 分区仍有约 929.8 KiB。录音和笔记保存在 microSD，不占用应用分区。

当前明确使用约 101 KiB PSRAM，其中约 96 KiB 是录音环形缓冲区，约 5 KiB 是墨水屏帧缓冲区。编译时必须保留 `PSRAM=opi`。

## 云端服务与隐私

- **硅基流动**：接收用户主动同步的 WAV，并返回转写文本；随后接收部分转写文本进行 AI 整理。
- **闪念贝壳**：接收标题、摘要、整理正文、原始转写片段和标签；不接收设备 WAV。
- **本地网页**：仅在设备进入传输模式时运行，通常只在同一局域网或设备热点内可访问。
- **旧版 GitHub/Obsidian**：源码中仍保留兼容实现，但收纳在配置页折叠区域，普通用户不需要启用。

不要在源码、Issue、截图或演示视频中公开 API Key、MCP Token、Wi-Fi 密码和个人笔记。已经公开过的凭据必须在服务商后台撤销并重新创建。

## 文档

- [完整中文使用说明书](./USER_GUIDE_ZH.md)
- [开源发布清单](./OPEN_SOURCE_CHECKLIST_ZH.md)
- [自定义 Flash 分区](./forrest_note/partitions.csv)
- [固件入口](./forrest_note/forrest_note.ino)
- [第三方组件与字体许可](./THIRD_PARTY_NOTICES.md)

## 当前限制

- 仅支持 2.4 GHz Wi-Fi；需要网页认证或 802.1X 的企业网络不可直接使用。
- 当前没有定时自动上传，必须在设备菜单中手动执行“同步”。
- 当前触摸版开发板的触摸层不响应。
- 飞书同步仍是界面预留项，尚未实现。
- 尚未实现从闪念贝壳搜索笔记并显示到设备。
- 超过约 25 分钟的 WAV 尚未自动分块转写。
- 提示音主题不控制录音播放音量，播放音量当前固定。

## 路线图

- 长录音云端分块转写、逐段总结和最终合并。
- 闪念贝壳搜索、读取和待办工具。
- 飞书文档或多维表格同步。
- 网页端修改笔记标题与标签。
- 可调录音回放音量。
- 更适合普通用户的本地 OTA 文件上传和正式固件 Release。

## 来源与致谢

记忆面包基于 [Forrest404/forrest-notes](https://github.com/Forrest404/forrest-notes) 继续开发。Forrest Notes 又建立在 Pala Note 的硬件适配、录音、墨水屏和设备交互基础上。

- Forrest Notes：<https://github.com/Forrest404/forrest-notes>
- Pala Note：<https://ko-fi.com/s/674a1a82e0>
- 微雪目标开发板：<https://www.waveshare.net/shop/ESP32-S3-ePaper-1.54-EN.htm>

请保留上游作者署名。原 Pala Note 下载包中的 3D 外壳文件不包含在本仓库中；如需外壳，请通过原项目获取。

## 许可证

固件源码沿用仓库中的 [MIT License](./LICENSE)。生成的 Memory Bread CJK Bitmap 使用 SIL OFL 1.1；其他第三方库、图像与硬件资料遵循各自的许可证和使用条款，详见[第三方组件说明](./THIRD_PARTY_NOTICES.md)。
