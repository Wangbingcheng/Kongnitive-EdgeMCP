# MCP Tools 对话记录

## 主题: tool_lua_get_script 参数、栈大小、内存等问题

---

### 1. tool_lua_get_script 被调用时传入的参数

**函数签名** (mcp_tools.c:689):
```c
static esp_err_t tool_lua_get_script(cJSON *args, char *result, size_t max_len)
```

**三个参数：**
1. `cJSON *args` - 从 JSON-RPC 请求中提取的 `arguments` 对象，包含 `name` 字段（脚本文件名）
2. `char *result` - 用于写入结果的输出缓冲区
3. `size_t max_len` - result 缓冲区的最大长度 = **2048 字节**

---

### 2. max_len 单位

单位是**字节**。

在 C 语言中，`sizeof(char)` 恒等于 1 字节。

---

### 3. 增大 max_len 是否需要改 FreeRTOS 任务栈

不需要改 FreeRTOS 任务栈大小。

`result_text[2048]` 是 `mcp_handle_tools_call` 函数内的局部变量，位于任务栈上。

当前配置：
- HTTP 服务器任务栈：8192 字节 (main.c:120)
- 当前 result_text 缓冲区：2048 字节

即使增大到 4KB、8KB，也小于 8KB 栈空间。

---

### 4. 增大 result_text 导致栈溢出

用户将 result_text 改为 8192 字节，并增大栈到 10240 字节，但仍然溢出。

**原因**：
- 10240 字节的栈需要容纳 8192 字节的数组 + 其他局部变量 + 函数调用帧
- 导致 "Stack protection fault"

**解决方案**：继续增大栈到 16384 字节

修改了两处：
- main.c:120 - HTTP 服务器栈: 16KB
- main.c:149 - HTTPS 服务器栈: 16KB

需要改两处是因为代码启动了两个不同的服务器：
1. `start_http_server()` - 端口 80，无 TLS
2. `start_mcp_server()` - 端口 443，带 SSL/TLS

---

### 5. SPIFFS 中 Lua 脚本占用空间

**SPIFFS 分区大小**: 0x40000 = **256 KB**

**Lua 脚本总大小**:
| 文件 | 大小 |
|------|------|
| default_main.lua | 4,935 字节 |
| default_provider_sht40.lua | 3,013 字节 |
| default_provider_ssd1306.lua | 1,755 字节 |
| default_di_container.lua | 1,007 字节 |
| default_bindings.lua | 241 字节 |
| **总计** | **10,951 字节 (≈10.7 KB)** |

SPIFFS 还剩约 245 KB 可用。

---

### 6. lua_runtime_list_scripts 函数

**功能**: 列出 SPIFFS 中所有 Lua 脚本

**工作流程** (lua_runtime.c:698-728):
1. 打开 SPIFFS 目录 (`/spiffs`)
2. 遍历目录中的文件
3. 获取每个文件的大小
4. 格式化输出到缓冲区：`filename (size bytes)\n`

**返回值示例**:
```
main.lua (4935 bytes)
default_main.lua (4935 bytes)
default_provider_sht40.lua (3013 bytes)
...
```

---

### 7. build_bindings_lua_script 函数

**功能**: 构建 bindings.lua 脚本，用于 DI（依赖注入）绑定

**函数签名** (mcp_tools.c:569):
```c
static bool build_bindings_lua_script(const char *interface_name, const char *provider,
                                      cJSON *opts, char *out, size_t out_len)
```

**输入参数**:
| 参数 | 说明 |
|------|------|
| interface_name | 接口名（默认 "display"） |
| provider | 提供者名（如 "ssd1306", "mock_display"） |
| opts | 可选的配置对象 |
| out | 输出缓冲区（2048字节） |
| out_len | 缓冲区大小 |

**输出格式示例**:
```lua
return {
    ["display"] = {
        provider = "ssd1306",
        opts = {width = 128, height = 64}
    }
}
```

**关键辅助函数**:
- `strbuf_append()` - 添加普通文本
- `strbuf_append_lua_string()` - 对字符串进行 Lua 转义处理
- `serialize_cjson_to_lua()` - 递归将 cJSON 转换为 Lua 语法

**调用链**:
```
MCP 客户端
  └─> tools/call {name: "lua_bind_dependency", arguments: {...}}
        └─> tool_lua_bind_dependency()
              └─> build_bindings_lua_script()
                    └─> lua_runtime_push_script("bindings.lua", ...)
                          └─> (可选) lua_runtime_restart()
```

**用途**: 运行时动态切换硬件抽象层实现，例如从真实硬件 ssd1306 切换到模拟 mock_display
