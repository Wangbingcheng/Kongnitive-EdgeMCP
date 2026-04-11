# AGENTS.md - Kongnitive EdgeMCP 开发者指南

本文件为在 Kongnitive EdgeMCP 项目上工作的 AI 代理提供开发指南。

## 项目概述

Kongnitive EdgeMCP 是一个运行在 ESP32 上的 MCP 服务器，内嵌 Lua 5.4 运行时。AI 代理可以通过 MCP 工具将 Lua 脚本写入 SPIFFS 来实时更新设备行为，无需重新构建固件。

- **语言**: C（固件）+ Lua（运行时脚本）
- **框架**: ESP-IDF v5.0+
- **目标硬件**: ESP32（已验证：Seeed Studio XIAO ESP32S3）

## 推荐工具

### 乐鑫文档 MCP 服务器
对于任何关于 ESP-IDF API、硬件细节或乐鑫框架的问题，请使用官方的乐鑫文档 MCP 服务器：
- **URL**: `https://mcp.espressif.com/docs`
- **功能**: 对所有乐鑫技术文档进行语义搜索。

## 构建命令

### 基本构建流程
```bash
# 导出 ESP-IDF 环境
. $HOME/esp/esp-idf/export.sh

# 构建项目
idf.py build

# 烧录到设备（替换 /dev/ttyUSB0 为你的端口）
idf.py -p /dev/ttyUSB0 flash monitor

# 在 Windows 上，使用 COM 端口：
idf.py -p COM3 flash monitor
```

### 单文件编译
```bash
# 编辑单个文件后重新构建
idf.py build

# 或直接使用 ninja 加快构建速度：
ninja -C build
```

### 清理构建
```bash
idf.py fullclean
idf.py build
```

### 配置
```bash
# 打开 menuconfig
idf.py menuconfig

# 使用自定义分区表构建
idf.py -p /dev/ttyUSB0 build -D partition_table=partitions_ota.csv
```

## 测试

本项目使用**设备上测试**而非单元测试。没有正式的测试框架。

### 冒烟测试流程
代码修改后，使用 MCP 工具在设备上验证：

1. **核心工具必须正常工作**：
   ```json
   {"method":"tools/call","params":{"name":"get_status","arguments":{}}}
   {"method":"tools/call","params":{"name":"sys_get_logs","arguments":{"lines":20}}}
   ```

2. **Lua 工具必须正常工作**：
   ```json
   {"method":"tools/call","params":{"name":"lua_list_scripts","arguments":{}}}
   {"method":"tools/call","params":{"name":"lua_exec","arguments":{"code":"return 1+1"}}}
   ```

3. **验证受影响工具无回归**

### 使用 curl 快速验证
```bash
curl -X POST http://<DEVICE_IP>/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

## 代码风格指南

### 语言
- 代码和文档使用**英语**
- C 标准：C99 或更高版本（ESP-IDF 兼容）

### 命名规范

| 元素 | 规范 | 示例 |
|------|------|------|
| 函数 | `小写加下划线` | `mcp_tools_init` |
| 变量 | `小写加下划线` | `led_initialized` |
| 全局变量 | `g_` 前缀 + 小写 | `g_tool_registry` |
| 常量 | `大写加下划线` | `MCP_MAX_RESULT_SIZE` |
| 类型/结构体 | `PascalCase` | `mcp_tool_t` |
| 枚举 | `PascalCase` + 值大写 | `ToolState_Idle` |
| 宏 | `大写加下划线` | `ESP_LOGI(TAG, ...)` |
| 静态常量 | `k` 前缀 + PascalCase | `static const char* kTag` |

### 文件组织

**头文件**（`*.h`）：
```c
#ifndef MODULE_NAME_H
#define MODULE_NAME_H

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 声明

#ifdef __cplusplus
}
#endif

#endif // MODULE_NAME_H
```

**源文件**（`*.c`）：
```c
/*
 * 模块描述
 */

#include "module_name.h"
#include <esp_log.h>

static const char *TAG = "module_name";

// 实现
```

### 头文件包含顺序
从最通用到最具体排序：
1. 标准 C 库（`<stdio.h>`、`<string.h>` 等）
2. ESP-IDF 头文件（`<esp_log.h>`、`<esp_err.h>` 等）
3. 第三方组件（`<cJSON.h>` 等）
4. 项目头文件（`"mcp_tools.h"`、`"lua_runtime.h"` 等）

### 大括号风格
- 使用 **K&R 风格**（埃及括号）：
```c
if (condition) {
    do_something();
} else {
    do_other();
}

while (condition) {
    iterate();
}

for (int i = 0; i < n; i++) {
    process(i);
}
```

### 注释
- 使用 **C89 块注释**（`/* */`），而非 C++ 风格（`//`）：
```c
/* 这是注释 */

/*
 * 多行
 * 注释
 */
```

- 为以下内容添加注释：
  - 函数目的和参数
  - 非显而易见的逻辑
  - 带有问题引用的 bug 修复说明

### 错误处理
- 函数返回类型使用 `esp_err_t`
- 检查返回值：`if (ret != ESP_OK) { ... }`
- 记录错误：`ESP_LOGE(TAG, "Failed: %s", esp_err_to_name(ret))`
- 传播错误：`return ret;`

### 日志
使用适当的日志级别：
```c
ESP_LOGV(TAG, "详细调试信息");  // Verbose
ESP_LOGD(TAG, "调试信息");      // Debug
ESP_LOGI(TAG, "信息");          // Info（默认）
ESP_LOGW(TAG, "警告: %s", msg); // Warnings
ESP_LOGE(TAG, "错误: %s", msg); // Errors
```

- 始终定义 `static const char *TAG = "module_name";`
- 避免记录敏感数据

### 内存管理
- 使用 ESP-IDF 堆函数：`esp_malloc()`、`esp_calloc()`
- 检查分配结果：`if (ptr == NULL) { return ESP_ERR_NO_MEM; }`
- 以相反顺序释放资源
- 谨慎使用 `ESP_ERROR_CHECK()`（它会在错误时重启）

## Git 工作流程

### 分支命名
- `main`：仅用于发布（受保护）
- `dev`：日常开发集成分支
- 功能分支：`feat/<范围>-<简短描述>`
- 修复分支：`fix/<范围>-<简短描述>`
- 文档分支：`docs/<范围>-<简短描述>`

示例：
- `feat/mcp-add-ota-status-fields`
- `fix/lua-runtime-memory-report`
- `docs/contribution-branch-policy`

### 提交信息风格
使用常规前缀：
- `feat:` 新功能
- `fix:` bug 修复
- `docs:` 仅文档
- `chore:` 维护/重构（无行为变更）
- `refactor:` 代码重构

示例：
```
feat: 添加 lua_bind_dependency 工具支持 DI 热切换

新增 MCP 工具用于更新 bindings.lua 并可选重启
Lua VM，实现无需固件更新即可切换运行时 provider。

Closes #42
```

### PR 检查清单
- [ ] 构建成功（`idf.py build`）
- [ ] 设备冒烟测试通过
- [ ] 核心工具无回归
- [ ] 文档已更新
- [ ] 无敏感信息或本地文件提交

## 文档要求

更改行为或工具时：

1. **更新 README.md** - 用 JSON-RPC 示例记录新工具
2. **更新 MCP_AGENT_CONFIG.md** - 如果代理工作流变更
3. **更新 doc/TODO.md** - 如果添加新 TODO
4. **更新本文件** - 如果编码标准变更

### MCP 工具添加检查清单
- [ ] 在 `main/mcp_tools.c` 的 `tool_registry[]` 中添加工具定义
- [ ] 实现处理函数
- [ ] 添加前向声明
- [ ] 在设备上测试
- [ ] 在 README.md 中用 JSON-RPC 示例记录

## Lua 脚本指南

运行时行为变更，优先使用 Lua 而非 C：

- 脚本放入 `/spiffs/`（SPIFFS 分区）
- 使用 DI 容器：`di_container.lua`、`bindings.lua`
- 配置保存在 bindings.lua（地址、引脚、选项）
- 使用 `lua_push_script` + `lua_restart` 部署

## 重要文件

| 文件 | 用途 |
|------|------|
| `main/mcp_tools.c` | MCP 工具注册表和处理器 |
| `main/mcp_server.c` | MCP HTTP 服务器实现 |
| `main/lua_runtime.c` | Lua 虚拟机管理 |
| `main/mcp_ota.c` | OTA 更新功能 |
| `main/mcp_log.c` | 日志缓冲区管理 |
| `doc/contribution.md` | 详细贡献指南 |

## 验证命令

始终在设备上验证更改：
```json
{"method":"tools/call","params":{"name":"sys_get_logs","arguments":{"lines":50}}}
{"method":"tools/call","params":{"name":"get_status","arguments":{}}}
```
