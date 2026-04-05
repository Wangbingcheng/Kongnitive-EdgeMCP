# AGENTS.md - Kongnitive EdgeMCP Developer Guide

This file provides guidelines for AI agents working on the Kongnitive EdgeMCP project.

## Project Overview

Kongnitive EdgeMCP is an ESP32 MCP server with an embedded Lua 5.4 runtime. AI agents can update device behavior at runtime by writing Lua scripts to SPIFFS without rebuilding firmware.

- **Language**: C (firmware) + Lua (runtime scripts)
- **Framework**: ESP-IDF v5.0+
- **Target Hardware**: ESP32 (verified: Seeed Studio XIAO ESP32S3)

## Build Commands

### ESP-IDF Version
This project uses **ESP-IDF v6.0**. Use the correct path:
```bash
# Correct path for v6.0
. /home/bing/.espressif/v6.0/esp-idf/export.sh
```

### Basic Build Workflow
```bash
# Export ESP-IDF environment
. $HOME/esp/esp-idf/export.sh

# Build the project
idf.py build

# Flash to device (replace /dev/ttyUSB0 with your port)
idf.py -p /dev/ttyUSB0 flash monitor

# On Windows, use COM port:
idf.py -p COM3 flash monitor
```

### Single File Compilation
```bash
# Rebuild after editing a single file
idf.py build

# Or use ninja directly for faster rebuilds:
ninja -C build
```

### Clean Build
```bash
idf.py fullclean
idf.py build
```

### Configuration
```bash
# Open menuconfig
idf.py menuconfig

# Build with custom partition table
idf.py -p /dev/ttyUSB0 build -D partition_table=partitions_ota.csv
```

## Testing

This project uses **on-device testing** rather than unit tests. There is no formal test framework.

### Smoke Test Procedure
After any code change, verify on device using MCP tools:

1. **Core tools must work**:
   ```json
   {"method":"tools/call","params":{"name":"get_status","arguments":{}}}
   {"method":"tools/call","params":{"name":"sys_get_logs","arguments":{"lines":20}}}
   ```

2. **Lua tools must work**:
   ```json
   {"method":"tools/call","params":{"name":"lua_list_scripts","arguments":{}}}
   {"method":"tools/call","params":{"name":"lua_exec","arguments":{"code":"return 1+1"}}}
   ```

3. **Verify no regressions** in affected tools

### Quick Verification via curl
```bash
curl -X POST http://<DEVICE_IP>/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

## Code Style Guidelines

### Language
- Keep code and documentation in **English**
- C standard: C99 or later (ESP-IDF compatible)

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Functions | `lowercase_with_underscores` | `mcp_tools_init` |
| Variables | `lowercase_with_underscores` | `led_initialized` |
| Global variables | `g_` prefix + lowercase | `g_tool_registry` |
| Constants | `UPPER_CASE` | `MCP_MAX_RESULT_SIZE` |
| Types/Structs | `PascalCase` | `mcp_tool_t` |
| Enums | `PascalCase` + values UPPER | `ToolState_Idle` |
| Macros | `UPPER_CASE` | `ESP_LOGI(TAG, ...)` |
| Static const | `k` prefix + PascalCase | `static const char* kTag` |

### File Organization

**Header Files** (`*.h`):
```c
#ifndef MODULE_NAME_H
#define MODULE_NAME_H

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Declarations

#ifdef __cplusplus
}
#endif

#endif // MODULE_NAME_H
```

**Source Files** (`*.c`):
```c
/*
 * Module Description
 */

#include "module_name.h"
#include <esp_log.h>

static const char *TAG = "module_name";

// Implementation
```

### Include Order
Order includes from most general to most specific:
1. Standard C library (`<stdio.h>`, `<string.h>`, etc.)
2. ESP-IDF headers (`<esp_log.h>`, `<esp_err.h>`, etc.)
3. Third-party components (`<cJSON.h>`, etc.)
4. Project headers (`"mcp_tools.h"`, `"lua_runtime.h"`, etc.)

### Bracing Style
- Use **K&R style** (Egyptian brackets):
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

### Comments
- Use **C89 block comments** (`/* */`), NOT C++ style (`//`):
```c
/* This is a comment */

/*
 * Multi-line
 * comment
 */
```

- Add comments for:
  - Function purpose and parameters
  - Non-obvious logic
  - Bug workarounds with issue reference

### Error Handling
- Use `esp_err_t` for function return types
- Check return values: `if (ret != ESP_OK) { ... }`
- Log errors: `ESP_LOGE(TAG, "Failed: %s", esp_err_to_name(ret))`
- Propagate errors: `return ret;`

### Logging
Use appropriate log levels:
```c
ESP_LOGV(TAG, "Verbose debug info");  // Verbose
ESP_LOGD(TAG, "Debug info");          // Debug
ESP_LOGI(TAG, "Informational");       // Info (default)
ESP_LOGW(TAG, "Warning: %s", msg);    // Warnings
ESP_LOGE(TAG, "Error: %s", msg);      // Errors
```

- Always define `static const char *TAG = "module_name";`
- Avoid logging sensitive data

### Memory Management
- Use ESP-IDF heap functions: `esp_malloc()`, `esp_calloc()`
- Check allocation: `if (ptr == NULL) { return ESP_ERR_NO_MEM; }`
- Free resources in reverse order of allocation
- Use `ESP_ERROR_CHECK()` sparingly (it reboots on error)

## Git Workflow

### Branch Naming
- `main`: Release-ready code only (protected)
- `dev`: Integration branch for daily development
- Feature branches: `feat/<scope>-<short-desc>`
- Fix branches: `fix/<scope>-<short-desc>`
- Documentation: `docs/<scope>-<short-desc>`

Examples:
- `feat/mcp-add-ota-status-fields`
- `fix/lua-runtime-memory-report`
- `docs/contribution-branch-policy`

### Commit Message Style
Use conventional prefixes:
- `feat:` new capability
- `fix:` bug fix
- `docs:` documentation only
- `chore:` maintenance/refactor without behavior change
- `refactor:` code restructuring

Example:
```
feat: add lua_bind_dependency tool for DI hot-switch

Add new MCP tool to update bindings.lua and optionally restart
Lua VM. This enables runtime provider switching without firmware
update.

Closes #42
```

### PR Checklist
- [ ] Build succeeds (`idf.py build`)
- [ ] Device smoke test passes
- [ ] No regressions in core tools
- [ ] Documentation updated
- [ ] No secrets or local files committed

## Documentation Requirements

When changing behavior or tools:

1. **Update README.md** - Document new tools with example calls
2. **Update MCP_AGENT_CONFIG.md** - If agent workflow changes
3. **Update doc/TODO.md** - If adding new TODOs
4. **Update this file** - If coding standards change

### MCP Tool Addition Checklist
- [ ] Add tool definition to `tool_registry[]` in `main/mcp_tools.c`
- [ ] Implement handler function
- [ ] Add forward declaration
- [ ] Test on device
- [ ] Document in README.md with JSON-RPC example

## Lua Script Guidelines

For runtime behavior changes, prefer Lua over C:

- Place scripts in `/spiffs/` (SPIFFS partition)
- Use DI container: `di_container.lua`, `bindings.lua`
- Keep configuration in bindings.lua (addresses, pins, options)
- Use `lua_push_script` + `lua_restart` for deployment

## Embedded Scripts

### How Embedded Scripts Work
- Default Lua scripts are embedded via `EMBED_TXTFILES` in `main/CMakeLists.txt`
- Scripts are converted to `.S` assembly with `.byte` directives
- Length is declared via `.long` (e.g., `.long 2579` = script length without null)
- Symbol `default_main_lua_length` provides the correct length at runtime

### Reading Embedded Script Length
```c
// Correct: use the length symbol (declared in .S file)
extern const uint32_t default_main_lua_length asm("default_main_lua_length");

// Wrong: end - start includes null terminator (2580 vs 2579)
size_t len = default_main_lua_end - default_main_lua_start; // WRONG
```

### Extracting Embedded Script for Debugging
```bash
# Extract from .S file (only .byte lines, exact length)
grep "^.byte" build/default_main.lua.S | sed 's/^\.byte //' | tr -d ' \n' | xxd -r -p | head -c 2579 > extracted.lua
```

## Important Files

| File | Purpose |
|------|---------|
| `main/mcp_tools.c` | MCP tool registry and handlers |
| `main/mcp_server.c` | MCP HTTP server implementation |
| `main/lua_runtime.c` | Lua VM management |
| `main/mcp_ota.c` | OTA update functionality |
| `main/mcp_log.c` | Log buffer management |
| `doc/contribution.md` | Detailed contribution guide |

## Verification Commands

Always verify changes on device:
```json
{"method":"tools/call","params":{"name":"sys_get_logs","arguments":{"lines":50}}}
{"method":"tools/call","params":{"name":"get_status","arguments":{}}}
```
