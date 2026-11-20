# 格式实现说明

openmd 的阅读器部分不依赖 .NET、Office、Adobe 或浏览器。下面记录每种格式的实现方式，以及可参考的开源方案。

## Markdown

- 自己实现块级解析：围栏代码、ATX/setext 标题、引用、列表、任务列表、表格、分隔线、图片、段落。
- 行内解析：粗体、斜体、删除线、行内代码、链接、自动链接、图片、HTML 实体、行内/独立 LaTeX。
- 代码高亮：按语言选择关键字/类型集合，输出 DirectWrite 彩色 run。
- 参考实现：
  - [cmark](https://github.com/commonmark/cmark) / [cmark-gfm](https://github.com/github/cmark-gfm)
  - [md4c](https://github.com/mity/md4c)
  - [KaTeX](https://katex.org/) / [MathJax](https://www.mathjax.org/)（公式语法的兼容性参考）

## LaTeX

`src/latex.cpp` 是一个轻量 math layout 引擎：

- 解析 `\frac`、`\sqrt`、上下标、`\left...\right`、矩阵环境、`\text`、重音符号等。
- 用 DirectWrite 测量 Cambria Math 文本，用 Direct2D 路径绘制分数线、根号、括号和矩阵。
- 行内公式通过 `IDWriteInlineObject` 嵌入普通文本布局，让 DirectWrite 负责换行和基线。
- 未知命令降级为普通文本；不加载远程字体，也不依赖 MathJax。

## DOCX

直接读取 OOXML：

1. `vendor/miniz.c` 解 ZIP；
2. `src/xml.cpp` 解析 `word/document.xml`、`styles.xml`、`numbering.xml` 和关系文件；
3. 还原标题、段落、粗体/斜体/下划线/删除线、行内代码、列表编号、引用、超链接、表格、图片；
4. 图片通过 `word/_rels/document.xml.rels` 的 rId 找到 `word/media/*`。

不依赖 Open XML SDK。可参考：

- [Open XML SDK](https://github.com/dotnet/Open-XML-SDK)
- [python-docx](https://github.com/python-openxml/python-docx)
- [Apache POI XWPF](https://poi.apache.org/components/document/)

## XLSX

同样通过 ZIP + XML：

- `xl/workbook.xml` 获取工作表名称和 rId；
- `xl/_rels/workbook.xml.rels` 解析工作表路径；
- `xl/sharedStrings.xml` 处理共享字符串；
- 解析 `sheetData/row/c`，支持 `t="s"`、`inlineStr`、数值、布尔和公式缓存值；
- 表格复用 CSV 的 grid 视图，支持多工作表标签。

## DOC（Word 97）

- `src/ole.cpp` 读取 OLE Compound File（FAT、MiniFAT、目录项、流）。
- `src/legacy.cpp` 读取 `WordDocument` 和 `1Table`/`0Table`，解析 FIB 与 CLX 片段表。
- 处理压缩/UTF-16 片段、字段指令、段落标记、单元格标记。
- 这是**兼容读取模式**：只提取正文，不还原原始分页、字体、图片和复杂样式。
- 可参考：
  - [Apache POI HWPF](https://poi.apache.org/components/document/)
  - [antiword](https://www.winfield.demon.nl/)
  - [LibreOffice](https://www.libreoffice.org/) 的 Word 97 导入实现

## XLS（Excel 97）

- 读取 `Workbook`/`Book` 流；
- 解析 BOUNDSHEET、SST/CONTINUE、LABELSST、LABEL、NUMBER、RK、MULRK、FORMULA、BOOLERR 等 BIFF8 记录；
- 处理共享字符串跨 CONTINUE 的边界标志；
- 只读值视图，不计算公式、不保留格式。
- 可参考：[Apache POI HSSF](https://poi.apache.org/components/spreadsheet/)。

## PDF

- 使用 [PDFium](https://pdfium.googlesource.com/pdfium/)（BSD 3-Clause）作为渲染引擎。
- `vendor/pdfium/pdfium.dll` 以 RCDATA 资源嵌入 exe；首次打开 PDF 时解压到 `%LOCALAPPDATA%\openmd\pdfium-8152.dll`，然后用 `LoadLibrary` + `GetProcAddress` 动态加载。也支持把 `pdfium.dll` 放在 exe 同目录。
- 每页通过 `FPDFBitmap_CreateEx` 渲染为 BGRA，再包装成 BMP，复用已有的 WIC → Direct2D 图片缓存。
- 这样不需要 Adobe Reader、Edge/WebView2、.NET，也不需要把 PDF 上传到任何地方。
- 其他可选开源引擎：
  - [PDFium](https://pdfium.googlesource.com/pdfium/)（BSD，本项目采用）
  - [MuPDF](https://mupdf.com/)（AGPL/商业）
  - [Poppler](https://poppler.freedesktop.org/)（GPL）

## 文件编码

`src/common.cpp` 的 `DecodeBytes`：

1. UTF-8 BOM / UTF-16 BOM；
2. 无 BOM UTF-16 零字节启发式；
3. 严格 UTF-8 校验；
4. GB18030；
5. Latin-1 兜底。

## 为什么不用 WebView2 / Electron

两者都会引入额外的运行时、较大的安装体积和潜在的网络/自动更新行为。openmd 的目标是离线、单文件、启动快，因此窗口、排版和交互都用原生 Win32 + Direct2D 完成。
