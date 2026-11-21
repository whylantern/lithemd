# LitheMD 逻辑问题解决方案

> 对应 `docs/PROJECT-MINDMAP.md` 中的审查结论。本文给出可落地的方案、改动范围、验收标准和实施顺序。
> 当前基线：2.4.1.4。

## 0. 总体原则

### 0.1 线程边界

```text
UI 线程
  窗口、Canvas、Direct2D、DirectWrite、鼠标键盘、标签页

Core 线程
  文档解析、ZIP 读取/解压、目录枚举、文件时间比较

消息回传
  Core 完成后只回传结构化结果，不直接操作 HWND
```

规则：

- Core 不包含 `HWND`、不绘制、不调用 `InvalidateRect`。
- UI 不执行文件解析、解压和递归目录扫描。
- 所有后台任务都带生成号或 token，能取消。
- 关闭窗口时，后台任务必须停止或安全放弃结果。

### 0.2 资源预算

| 资源 | 建议值 |
|---|---:|
| 工作区缓存条目 | 200,000 |
| 工作区队列 | 2,048 个目录 |
| 单个文档 | 512 MB |
| ZIP 单条目 | 512 MB |
| ZIP 总量 | 4 GB |
| 笔记文件 | 64 MB |
| 图片像素 | 100 MP |
| 导出预览 | 现有上限保持 |

预算超过时停止“继续加载”，但不静默隐藏当前目录内容。

---

## 1. H1：文档同步加载导致 UI 卡顿

### 方案

新增异步打开任务：

```text
OpenFile
  -> 创建标签页，显示“正在打开…”
  -> 提交 LoadJob 到后台
  -> 后台 LoadDocument
  -> PostMessage 回传 Document / 错误 / 编码信息
  -> UI 线程 SetDocument 或 SetError
```

### 关键设计

```cpp
struct LoadRequest {
    uint64_t token;
    std::wstring path;
    TextEncoding encoding;
    DocLanguage language;
    bool repairMojibake;
};

struct LoadResult {
    uint64_t token;
    std::unique_ptr<Document> doc;
    std::wstring error;
    TextEncoding encoding;
    DocLanguage language;
};
```

- `Tab` 增加 `uint64_t loadToken`。
- 关闭标签时 token 失效，回传结果丢弃。
- 再次打开同一路径时复用现有 loading 标签。
- 后台线程负责纯解析；Direct2D/DirectWrite 仍只在 UI 使用。
- 图片尺寸查询使用独立 WIC/COM，后台线程先 `CoInitializeEx`。
- PDF 的文件头检查可在后台；PDFium 页面加载仍在 UI 侧或单独的任务。
- 解析完成后只通过 `PostMessage` 回传，不跨线程碰 `DocView`。

### 验收

- 打开 200 MB 文本/Office 文件时窗口仍可拖动、切换标签。
- 打开过程显示“正在打开…”，可取消。
- 返回后渲染结果与同步加载一致。
- 关闭 loading 标签不会崩溃或泄漏结果。

### 优先级

P0，建议 2.4.2.0。

---

## 2. H2/H3：ZIP 打开、预览、单条解压同步

### 方案

复用现有后台线程模式：

#### 打开 ZIP

```text
创建 ArchiveWindow loading 态
  -> 后台 ArchiveReader::Open
  -> 回传条目列表
  -> UI 填充列表
```

#### 预览条目

```text
读取条目（后台）
  -> 解密/解压
  -> 写入临时文件
  -> 回传临时路径
  -> UI 打开预览标签
```

#### 解压选中

```text
后台读条目 -> 写入目标文件
```

#### 全部解压

保留现有后台线程，补上：

- 进度
- 取消
- 关闭窗口时的取消
- 目标已存在时的确认

### 关键设计

- `ArchiveReader` 继续用 `shared_ptr` 持有，后台任务不依赖 `ArchiveWindow` 生命周期。
- 密码只在 UI 线程输入，后台任务拿到密码副本，任务结束立即释放。
- 结果为结构化数据 + 错误码，UI 负责提示。
- 预览临时文件按归档路径哈希复用。

### 验收

- 打开 500 MB ZIP 时窗口不冻结。
- 预览 64 MB 条目时 UI 可响应。
- 单条解压可取消。
- 关闭归档窗口后后台任务不访问已销毁控件。

### 优先级

P0，2.4.2.0。

---

## 3. H4：扫描队列可能过大

### 问题

`WorkspaceScanner::ScanDirectory` 会把当前目录的所有子目录一次性入队。十万级目录会产生十万个 `Job`，缓存预算管不到队列内存。

### 方案 A：批量入队（推荐）

```text
ScanDirectory
  -> 只取前 N 个子目录入队（例如 32）
  -> 其余目录留在缓存中，作为 frontier
  -> 当前队列空后，由 ContinueIdle 继续取下一批
```

不再一次性构造全部 `Job`。

### 方案 B：显式 frontier 队列

```cpp
std::deque<std::wstring> frontier_;
std::set<std::wstring> pending_; // 去重
```

规则：

- 用户展开目录：优先插队，最多 32 个。
- 根预加载：只把前 32 个子目录放入活动队列。
- `ContinueIdle` 每批从 frontier 取 32 个。
- 队列总量上限 2048；超过后暂停深层预加载。

### 验收

- 10 万子目录的根目录不会一次性创建 10 万 Queue Job。
- 展开某个子目录仍然立即处理。
- 内存不随队列无限增长。

### 优先级

P1，2.4.2.0。

---

## 4. H5：超大目录节点和路径内存

### 问题

当前 `WorkspaceNode` 每个节点保存：

```cpp
std::wstring name;
std::wstring path;
std::vector<std::unique_ptr<Node>> children;
```

路径在每个节点重复，百万节点会占用大量内存。

### 方案：虚拟化工作区

分两步：

#### 第一步：减少复制

- 节点只保存 `name` 和父节点索引。
- `path` 按需计算，或只在行被点击时生成。
- `WorkspaceRow` 使用 `std::wstring_view` 或索引，不复制路径。
- `Rows()` 改为返回引用/切片，不返回新 vector。

#### 第二步：虚拟列表

```text
WorkspaceView
  -> 维护可见行索引
  -> 只绘制可见区域
  -> 只构建可见行文本
```

`WorkspaceModel` 维护：

```cpp
std::vector<VisibleEntry> flat_;
```

`VisibleEntry` 只保存节点索引和深度，不保存字符串。

### 短期折中

- 单目录超过 20 万项时显示明确提示。
- 不静默截断，但暂停自动展开/预加载。
- 当前目录仍可浏览。

### 验收

- 50 万项目录不会因为 `Rows()` 复制而卡死。
- 滚动只处理可见行。
- 点击、展开、活动高亮仍正常。

### 优先级

P1，2.4.2.0。

---

## 5. H6：`Rows()` 每次 Paint 全量复制

### 方案

当前：

```cpp
std::vector<WorkspaceRow> Rows() const;
```

改为：

```cpp
void RebuildVisibleRows();
const std::vector<WorkspaceRow>& Rows() const;
```

规则：

- 节点展开、缓存填充、刷新时重建一次。
- `WorkspaceView::Paint` 只读取缓存，不再复制。
- `WorkspaceView` 只绘制可见行。
- 大列表使用 `std::deque` 或索引切片，避免整表移动。

### 验收

- 展开 5000 项目录后连续滚动，CPU 不持续升高。
- Paint 期间不分配大块 vector。

### 优先级

P1，2.4.2.0。

---

## 6. M1：空闲扫描没有网络盘限制

### 方案

在 `WorkspaceScanner` 增加路径类型判断：

```text
GetDriveTypeW
PathIsNetworkPath
GetFileAttributes
```

规则：

- 本地固定盘：允许空闲扫描。
- 网络盘：默认只扫描用户展开的目录。
- 可移动盘：默认不进行空闲扫描。
- 用户可显式点击“刷新”或“加载更多”。

### 验收

- 打开网络工作区不会自动遍历整个共享目录。
- 手动展开仍然可用。

### 优先级

P2。

---

## 7. M2：同一个 ZIP 多窗口

### 方案

在 `MainWindow` 维护：

```cpp
std::map<std::wstring, HWND> archiveWindows_;
```

- 路径统一 `AbsolutePath + ToLower`。
- 已存在则 `SetForegroundWindow`。
- 关闭时移除。

`ArchiveWindow` 关闭回调通知主窗口，不再只依赖自删除。

### 验收

- 重复打开同一 ZIP 只激活已有窗口。
- 不同 ZIP 可以同时打开。

### 优先级

P2。

---

## 8. M3：CLI 路径规则不一致

### 方案

新增统一入口：

```cpp
OpenFileInput(path, baseDir, addRecent, activate);
```

或让 CLI 入口调用：

```cpp
ResolveUserPath(arg, ExeDir())
```

规则：

- CLI：支持 `%ENV%`、`~`、`file://`、引号。
- 拖放：只做绝对路径规范化，不展开环境变量。
- 路径栏：继续使用 `ResolveUserPath`。
- 最近记录：按规范化路径比较。

### 验收

```powershell
lithemd.exe "%USERPROFILE%\Documents\a.md"
lithemd.exe "~/Documents/a.md"
lithemd.exe "file:///C:/Docs/a.md"
```

三种写法都能打开。

### 优先级

P2。

---

## 9. M4：`Locate` 同步加载

### 方案

短期：

- 仅允许 `Locate` 在路径已经缓存或已加载时立即定位。
- 未加载时显示“正在定位…”，后台加载路径链。

中期：

- 使用和第 1 节相同的异步任务框架。
- 加载完成后高亮并滚动到目标行。

### 验收

- 在超大目录中定位文件时 UI 不冻结。
- 用户可以取消定位。

### 优先级

P2。

---

## 10. M5：权限失败显示为空目录

### 方案

在 `WorkspaceNode` / `WorkspaceRow` 增加：

```cpp
bool error;
std::wstring errorText;
```

扫描结果区分：

- 空目录：`complete=true, entries empty`
- 读取失败：`error=true`

UI 显示：

```text
无法读取该文件夹
```

而不是显示为空。

### 优先级

P2。

---

## 11. M6：刷新只恢复一级展开

### 方案

当前是保守策略。若要保留更深展开：

- 刷新前记录已展开路径。
- 只恢复可见展开路径（最多 32 个）。
- 每个恢复的目录通过缓存或后台加载其直接子项。
- 不递归加载所有后代。

### 验收

- 刷新后用户看到原来的可见层级。
- 不因恢复展开而扫描整棵树。

### 优先级

P2。

---

## 12. M7：PDF 密码不支持

### 方案

`PdfDocument::Load` 增加密码参数：

```cpp
bool Load(path, password, error);
```

流程：

1. 首次加载失败并返回 `FPDF_ERR_PASSWORD`。
2. UI 弹出密码输入框。
3. 使用密码重新加载。
4. 密码只保存在当前文档内存，不写配置。
5. 错误密码提示后允许重试或取消。

### 验收

- 加密 PDF 可以输入密码打开。
- 错误密码不会崩溃。
- 密码不写入最近记录、笔记或日志。

### 优先级

P2。

---

## 13. M8：文件监听只覆盖活动目录

### 方案

两种选择：

#### 方案 A：维持现状

- 只监听活动文档目录。
- 非活动标签在 `WM_ACTIVATE` 时比较时间。
- 成本最低，推荐保持。

#### 方案 B：多目录监听

- 按目录维护 watcher。
- `map<directory, FileWatcher>`。
- 文件变化消息带目录，主窗口检查对应标签。
- 复杂度明显增加。

### 建议

维持 A，除非用户明确要求后台标签实时刷新。

---

## 14. M9：文档加载无法取消

### 方案

随异步加载框架一起实现：

- `LoadJob` 带 `std::atomic<bool> cancel`。
- 解析器在 Markdown/Office/PDF 循环中定期检查。
- UI 提供取消按钮。
- 关闭标签时自动取消。

### 验收

- 打开大文件后可以取消。
- 取消后不创建半成品标签。

---

## 15. M10：Markdown 行内嵌套无深度上限

### 方案

`InlineParser::Parse` 增加参数：

```cpp
int depth = 0;
if (depth > 64) {
    // 将剩余内容按普通文本处理
}
```

递归调用统一传递 `depth + 1`。

覆盖：

- 强调/斜体
- 链接
- 图片
- 删除线
- 高亮/上下标

### 验收

- 10000 层嵌套强调不会栈溢出。
- 普通文档渲染结果不变。

### 优先级

P2。

---

## 16. M11：图片解码内存预算

### 现状

`render.cpp` 已经有两个检查点，并设置了 `kMaxImagePixels = 100 MP`：

- `ImagePixelSize()` 解码前查询尺寸；
- `Canvas::Image()` 创建 D2D 位图前再次检查。

因此“完全没有上限”并不准确。剩余问题是：通过检查后仍按原始尺寸解码，100 MP 的 32bpp 图片理论上需要约 400 MB；多标签或背景图会进一步放大。

### 方案

保留 100 MP 硬上限，并增加按显示尺寸解码：

```text
IWICBitmapFrameDecode::GetSize
  -> 超过 100 MP：拒绝
  -> 超过 50 MP：标记为大图，后台解码
  -> 按视口/显示区域计算目标尺寸
  -> 使用 IWICBitmapScaler 或 CopyPixels 下采样
  -> 只把下采样结果交给 D2D
```

规则：

- 图片查看按窗口尺寸和缩放比例下采样，不保留原始全尺寸位图。
- 文档内嵌图片按布局后的显示尺寸下采样。
- 背景图限制更严格：文件 64 MB、像素 6,000 万，并缩放到视口 2 倍以内。
- 解码失败或超限时显示占位提示，不阻塞文档打开。

### 验收

- 100 MP 图片被拒绝或下采样后打开，不出现数百 MB 的位图分配。
- 普通截图、照片正常打开。
- 多标签切换时内存不会按原始图片尺寸线性增长。

### 优先级

P2。

---

## 17. M12：关闭时等待扫描线程

### 方案

- 扫描线程检查 `stop_` / generation，每 256 个条目退出。
- 关闭目录句柄，唤醒线程。
- 主窗口退出最多等待 1–3 秒。
- 超时后放弃线程（进程退出时由系统回收），不要无限卡住 UI。

不要在 UI 线程无限 `join()` 网络路径扫描。

### 验收

- 关闭工作区/窗口通常 3 秒内完成。
- 网络盘不可用时不阻塞退出。

### 优先级

P1。

---

## 18. 实施顺序

### 2.4.2.0：异步打开与线程安全

1. 异步文档加载框架（H1）。
2. ZIP 打开/预览/单条解压异步（H2/H3）。
3. 文档加载取消（M9）。
4. 图片下采样与内存预算（M11）。
5. 扫描线程退出保护（M12）。

### 2.4.2.1：工作区内存与队列

1. 扫描队列分批/frontier（H4）。
2. `Rows()` 缓存（H6）。
3. 工作区虚拟化第一阶段（H5）。
4. 网络盘空闲扫描限制（M1）。
5. 权限错误行（M5）。

### 2.4.2.2：打开体验

1. ZIP 窗口去重（M2）。
2. CLI 路径统一（M3）。
3. 异步 Locate（M4）。
4. PDF 密码（M7）。
5. 刷新展开策略（M6）。
6. Markdown 行内深度（M10）。

---

## 19. 验收总表

| 场景 | 目标 |
|---|---|
| 200 MB Markdown | 打开不卡 UI，可取消 |
| 500 MB ZIP | 打开/预览不卡 UI |
| 10 万子目录 | 扫描队列不爆炸 |
| 50 万项目录 | 滚动不全量复制 |
| 网络工作区 | 不自动全盘扫描 |
| 重复 ZIP | 复用窗口 |
| CLI `%USERPROFILE%` | 可打开 |
| 加密 PDF | 可输入密码 |
| 10000 层 Markdown | 不栈溢出 |
| 超大图片 | 有内存上限 |
| 关闭扫描 | 不无限等待 |

---

## 20. 结论

当前最值得立即做的是：

1. **异步文档和 ZIP 打开**
2. **扫描队列上限**
3. **Rows 缓存和虚拟列表**

这三项解决后，项目的“单 exe、离线、轻量”定位可以继续保持，同时不会因为大文件、大目录或网络目录出现明显卡死。
