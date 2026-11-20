# LitheMD 演示文档

一个**离线**、轻量的本地文档阅读器。支持 *斜体*、~~删除线~~、`行内代码`、==高亮==、脚注[^1]，以及[外部链接](https://example.com)。

[^1]: 脚注示例：LitheMD 不依赖 WebView 或 .NET。

> 引用块：所有内容只在本机解析，不会上传到任何地方。
>
> 第二段引用，含 `inline code`。

## 目录与标题

### 列表

- 支持**加粗**与*强调*
- 嵌套列表
  - 第二层
    - 第三层
- 任务清单
  - [x] Markdown 渲染
  - [ ] 更多主题

1. 有序列表
2. 第二项
3. 第三项

### 表格

| 功能 | 支持 | 备注 |
|:-----|:----:|-----:|
| Markdown | 是 | 标题 / 列表 / 表格 |
| JSON | 是 | 树形 + 源码 |
| 离线 | 是 | 无网络请求 |

### 代码

```cpp
#include <bits/stdc++.h>
using namespace std;

// 快速幂
long long qpow(long long a, long long b, long long p) {
    long long r = 1;
    while (b) { if (b & 1) r = r * a % p; a = a * a % p; b >>= 1; }
    return r;
}
```

```json
{ "lithemd": true, "format": ["md", "json", "zip"], "size": "9 MB" }
```

```python
def hello(name: str) -> str:
    return f"你好，{name}！"  # f-string
```

---

## JSON 数据示例

打开同目录下的 [demo.json](demo.json) 查看树形视图。

### 快捷键

| 快捷键 | 作用 |
|---|---|
| Ctrl+O | 打开文件 |
| Ctrl+L | 输入路径 |
| Ctrl+Shift+O | 打开文件夹 |
| Ctrl+1 / Ctrl+2 | 预览 / 源码 |
| Ctrl+B | 大纲 |
| F11 | 演示模式 |
| Ctrl+滚轮 | 缩放 |
