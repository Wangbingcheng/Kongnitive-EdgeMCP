# ESP32 内存分配器详解

## TLSF（Two-Level Segregated Fit）

TLSF 是 ESP-IDF 使用的内存分配器，专为实时嵌入式系统设计。

### 核心特性

1. **O(1) 时间复杂度**：两级索引结构，查找空闲块恒定时间
2. **自动合并**：释放内存时自动合并相邻空闲块
3. **低碎片率**：按大小分类存放，优化内存利用
4. **确定性**：分配时间可预测，适合 RTOS

### 工作原理

```
┌─────────────────────────────────────────────────┐
│  空闲块按大小分类存储                           │
│  0-128B │ 128-256B │ 256-512B │ 512-1KB │ ...  │
│  索引:  fl * 32 + sl → O(1) 查找               │
└─────────────────────────────────────────────────┘
```

### 分配/释放流程

```c
// 分配：malloc → 查找空闲块 → 分割（如需要）→ 返回
// 释放：free → 检查相邻块 → 自动合并 → 放回链表
```

### ESP-IDF 中的实现

```c
// free() 内部调用链
free(ptr)
  └─> heap_caps_free(ptr)
       └─> TLSF 内部自动完成：
           - 合并相邻空闲块
           - 维护空闲链表
           - 更新内存统计
```

### malloc_trim 的问题

| 函数 | Linux | ESP-IDF |
|-----|-------|---------|
| malloc_trim | 归还内存给 OS | 桩函数（返回 0） |
| 原因 | 有 OS 支持 | 嵌入式无法归还物理内存 |

ESP-IDF 中的 `malloc_trim` 是空实现：
```c
int malloc_trim(size_t pad) {
    return 0;  // 表示失败
}
```

### 为什么不需要手动内存整理

1. TLSF 在每次 `free()` 时自动合并
2. 无法像 Linux 那样归还内存给硬件
3. 碎片化由分配器内部处理

### 调试命令

```bash
# 打印堆信息
heap_caps_print_heap_info(MALLOC_CAP_8BIT)

# 或通过 MCP 工具
lua_exec("return sys.heap_info()")
```

### 相关代码位置

- 分配器配置：`components/heap`
- Lua 堆统计：`main/lua_runtime.c` 的 `lua_mem_update()`