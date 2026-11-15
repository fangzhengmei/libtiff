# 损坏 IFD（Image File Directory）处理诊断报告

- 代码库：libtiff（分支 `task/62`，版本 4.7 开发树，`libtiff.so.6.3.0`）
- 分析对象：畸形 TIFF 中 IFD 条目计数、字段类型、条目偏移、下一 IFD 指针互相矛盾时的处理
- 测试方式：本地构造 24 个样本（20 个 Classic TIFF + 4 个 BigTIFF/未知类型样本），用
  ASan+UBSan 插桩构建的库在 **mmap（`"r"`）与流式（`"rm"`）两条 I/O 路径** 上验证
- 工作目录：`corrupt-ifd-work/`（脚本与样本分离，Python 均以 `python3 -I` 运行）

---

## 1. 代码诊断：四个关键环节如何处理畸形输入

### 1.1 目录读取（`TIFFFetchDirectory` / `TIFFReadDirectory`）

调用链：`TIFFOpen()`（`libtiff/tif_open.c:774`）内部就会调用一次
`TIFFReadDirectory()`（`libtiff/tif_dirread.c:4335`），后者先做 IFD 环检查
（`_TIFFCheckDirNumberAndOffset`，`tif_dirread.c:5781`），再调用
`TIFFFetchDirectory()`（`tif_dirread.c:6094`）把原始目录读入 `TIFFDirEntry` 数组。

**短目录（条目计数小于实际条目数）**

- 计数本身没有下限校验，库按伪造的计数只解析前 N 个条目，随后从
  `diroff + 2 + N*12` 处读取“下一 IFD 指针”。当计数被改小时，这个位置落在某个
  条目的中间，读到的是条目的 tag/type 半字拼成的垃圾偏移。
  实测：把 IFD1 计数从 9 改成 1 后，`tif_nextdiroff` 变成 `196865 (0x30061)`，
  正是偏移 136 处 `tag=0x0101, type=0x0003` 四个字节的小端拼接值。
- 解析出的少量条目随后在一致性校验阶段失败（缺必需的 `StripOffsets`），
  `TIFFReadDirectory()` 返回 0，但**垃圾 next 指针已写入句柄**（见第 3 节）。

**溢出计数（计数超过 sanity 上限）**

- Classic 与 BigTIFF、mmap 与流式四条路径都有 `dircount > 4096` 的 sanity 检查
  （`tif_dirread.c:6123`、`6144`、`6225`、`6251` 一带），超限直接报错返回 0，
  不会分配巨型数组。
- mmap 路径还有两道额外防线：
  1. `allocsize = dircount * dirsize > filesize` 时拒绝分配（`tif_dirread.c:6301`），
     实测对截断文件输出 warning `Requested memory size ... greater than filesize`；
  2. 读取前用 `m = off + dircount*dirsize` 做加法溢出与越界检查
     （`tif_dirread.c:6286-6293`），防止越界读。
- 流式路径不检查 0 计数（mmap 路径在 `tif_dirread.c:6302` 检查）。本报告的修复
  在流式路径补上了对齐的 0 计数拒绝。

**整数截断**

- BigTIFF 的 64 位计数先过 `>4096` 检查，再被强转为 `uint16_t`
  （`tif_dirread.c:6160` 与 `6287`，`dircount16 = (uint16_t)dircount64`）。
  当前 4096 上限使截断不可利用，但这是一个**隐式依赖**：一旦上调 sanity 上限，
  静默截断就会复活。修复中增加了显式的 `>0xFFFF` 拒绝（见第 4 节）。
- 标签数组侧另有独立防线：`TIFFReadDirEntryArrayWithLimit()`
  （`tif_dirread.c:1260`）用 `MAX_SIZE_TAG_DATA=2GiB` 限制单标签数据量，
  `count` 收窄为 `uint32_t` 前先做除法检查；`datasize` 超过 100 MiB 时还要与
  文件大小比较，避免损坏文件触发巨量分配。
- `EvaluateIFDdatasizeReading()`（`tif_dirread.c:4185`）对
  `tdir_count * TIFFDataWidth(type)` 做乘法溢出检查。

### 1.2 字段转换（条目类型与 count 的校验）

- 类型宽度表 `TIFFDataWidth()`（`libtiff/tif_dirinfo.c:695`）对未知类型返回 0。
  后果分两种：
  1. **未知 tag + 未知类型**：`TIFFReadDirectory` 为其动态创建匿名字段
     `_TIFFCreateAnonField()`（`tif_dirinfo.c:995`），default 分支把
     `set_get_field_type` 置为 `TIFF_SETGET_UNDEFINED`；后续
     `TIFFFetchNormalTag()`（`tif_dirread.c:6405`）只报一条 error
     “tag is not read from file”，**目录整体仍成功、图像照常解码**。
  2. **已知 tag + 未知/矛盾类型**：具名读取函数（如
     `TIFFReadDirEntryShort`）的 `switch` 走 default 返回
     `TIFFReadDirEntryErrType`，由 `TIFFReadDirEntryOutputErr()`
     （`tif_dirread.c:4015`）输出 `Incompatible type`，必需标签失败 →
     整个目录读取返回 0。
- count 不符返回 `TIFFReadDirEntryErrCount`；外部数据读失败返回
  `TIFFReadDirEntryErrIo`。是否致命取决于标签：第一遍处理几何/条带标签时
  （`tif_dirread.c:4559-4571`）任何失败都 `goto bad`；第二遍普通标签用
  `recover=TRUE` 调用（`tif_dirread.c:4883/4897/5543/5547`），失败仅 warning
  并忽略该标签。
- 条目值/外部数据偏移越界：内联值从 12/20 字节条目内拷贝（天然受限）；外部数据
  经 `TIFFReadDirEntryData()`（`tif_dirread.c:3988`）读取，mmap 路径显式检查
  `offset+size > tif_size`，流式路径靠短读返回 `ErrIo`。实测把 `StripOffsets`
  改成 `0xFFFFFFF0` 时，目录读取成功，错误推迟到解码阶段的 `TIFFFillStrip`
  报 `Read error on strip 0`。

### 1.3 下一 IFD 跳转与目录循环

- 跳转前的唯一闸门是 `_TIFFCheckDirNumberAndOffset()`（`tif_dirread.c:5781`）：
  它维护两张哈希表（offset→目录号、目录号→offset）。新偏移若已存在且目录号不同，
  判定为 IFD 循环，输出 warning
  `TIFF directory N has IFD looping to directory M ...` 并返回 0；
  同时 `TIFF_MAX_DIR_COUNT=1048576`（`libtiff/tiffiop.h:62`）限制链长，
  杜绝无环但超长的链造成资源耗尽。
- 随机访问路径 `TIFFSetDirectory()`（`libtiff/tif_dir.c:2094`）与
  `TIFFAdvanceDirectory()`（`tif_dir.c:1874`）复用同一套哈希表，
  循环指针在顺序读和随机读两条路径上都会被拦截。
- 实测三类循环全部终止且只产生 warning、不产生 error：
  自环（IFD0→IFD0）、双节点环（IFD1→IFD0）、尾 IFD 自环。
- 尾部指针指向 EOF 之后或像素区：在 fetch 阶段因 count 读失败或
  `>4096` sanity 失败而中止，`tif_nextdiroff` 被置 0，链干净终止。

### 1.4 图像解码入口

- `TIFFReadRGBAImage*()` 先经 `TIFFRGBAImageOK()`（`libtiff/tif_getimage.c:84`）
  与 `TIFFRGBAImageBegin()`（`tif_getimage.c:311`）校验
  bits/sample、photometric、色道数等，再由 `gtStripContig/gtTileContig`
  经 `TIFFFillStrip`（`libtiff/tif_read.c`）取数据。
- 解码层不重新验证 IFD，**完全信任 `tif_dir` 的当前内容**。因此
  `TIFFReadDirectory()` 失败后句柄上残留什么，解码就会用什么——这是第 3 节
  脏状态问题的根源。
- 条带越界由 `TIFFFillStrip` 的读长度校验兜住（实测返回明确 Read error，
  ASan 下无越界读），畸形几何（0 宽/0 高）在 `TIFFReadDirectory` 尾部的
  zero scanline/strip size 检查处被拒（`tif_dirread.c:5278-5301`）。

---

## 2. 本地样本与实测结果

### 2.1 样本构造

生成器：`corrupt-ifd-work/scripts/gen_samples.py`、`gen_extra.py`（纯标准库）。
基线文件为两个合法 9 条目 IFD，各描述 4×4、8-bit、灰度、无压缩图像
（IFD0 像素为 0..15 渐变，IFD1 像素恒为 128），所有损坏均通过定点覆写产生，
保留原始字节以便对比。

| 类别 | 样本 | 损坏手法 |
|---|---|---|
| 合法基线 | `valid_multi.tif`、`three_ifd_all_valid.tif`、`bigtiff_valid.tif` | 无损坏 |
| 损坏首个 IFD | `ifd0_short_count.tif` | 计数 9→1 |
| | `ifd0_overflow_count.tif` | 计数→5000（>4096） |
| | `ifd0_unknown_type.tif` | ImageWidth 类型→99 |
| | `ifd0_entry_oob.tif` | StripOffsets→0xFFFFFFF0 |
| | `ifd0_self_loop.tif` | IFD0 next→IFD0 |
| | `ifd0_truncated.tif` | 文件截断到计数词之后 |
| 损坏中间 IFD | `ifd1_short_count / overflow_count / unknown_type / entry_oob / truncated.tif` | 同上，作用于 IFD1 |
| | `three_ifd_middle_{short,unknowntype,overflow}.tif` | 三 IFD 链，仅损坏 IFD1，IFD2 合法 |
| 损坏尾部指针 | `tail_loop_to_ifd0.tif`、`tail_self_loop.tif` | 形成环 |
| | `tail_ptr_past_eof.tif` | next→EOF+64 |
| | `tail_ptr_into_pixels.tif` | next→像素区中间 |
| 边界 | `unknown_tag_unknown_type.tif` | 未知 tag 65000 + 未知类型 99 |
| | `bigtiff_overflow_count.tif`、`bigtiff_tail_past_eof.tif` | BigTIFF 64 位路径 |

### 2.2 实测结果（修复后库，mmap 与 stream 结论一致）

完整日志：`corrupt-ifd-work/results/harness_run.log`、`extra_run.log`、
`tiffinfo_run.log`。全部 24 个样本在 ASan/UBSan 下**零崩溃、零越界报告**。

| 样本（代表） | 库返回 / 错误码路径 | 诊断日志（节选） | 像素结果 |
|---|---|---|---|
| `valid_multi.tif` | 两个 IFD 均返回 1 | 无 | dir0 `0xff0c0c0c..` 渐变；dir1 恒 `0xff808080` |
| `ifd0_short_count.tif` | `TIFFOpen` 失败（ReadDirectory 返回 0） | ERROR `missing required "StripOffsets"` | 无目录可解码 |
| `ifd0_overflow_count.tif` | open 失败，2 条 error | ERROR `Sanity check on directory count failed` + `Failed to read directory at offset 8` | 无 |
| `ifd0_unknown_type.tif` | open 失败 | ERROR `Incompatible type for "ImageWidth"` | 无 |
| `ifd0_entry_oob.tif` | open 成功，解码返回 0 | ERROR（mmap）`Read error on strip 0; got 18446744069414584604 bytes` /（stream）`Read error at scanline 4294967295` | 解码失败，IFD1 仍正常（`0xff808080`） |
| `ifd0_self_loop.tif` | 第二次 ReadDirectory 返回 0 | WARN `IFD looping to directory 0 at offset 0x8` | dir0 正常解码 |
| `ifd0_truncated.tif` | open 失败 | mmap：WARN 分配超文件大小 + ERROR；stream：ERROR `Can not read TIFF directory` | 无 |
| `ifd1_*`（中间损坏） | dir0 返回 1，dir1 返回 0 | 与首 IFD 同类错误 | dir0 像素正确；链在 dir1 中止 |
| `ifd1_short_count.tif` | 同上，且 **next 指针被污染为 196865** | ERROR missing StripOffsets；随后 `Failed to read directory at offset 196865` | 仅 dir0 可解码 |
| `three_ifd_middle_overflow.tif` | IFD1 fetch 失败，IFD2 不可顺序到达 | 2× ERROR sanity/failed | dir0 正常；`SetDirectory(2)` 也失败（链已断） |
| `three_ifd_middle_unknowntype.tif` | IFD1 解析失败，但 next=268 保留 | ERROR `Incompatible type for "ImageLength"` | dir0 正常；`SetDirectory(2)` 仍可到达合法 IFD2 |
| `three_ifd_middle_short.tif` | IFD1 失败，next 被污染 | ERROR + 垃圾偏移 196865 | 合法 IFD2 被孤立 |
| `tail_loop_to_ifd0.tif` | 第三次读取返回 0 | WARN `directory 1 has IFD looping to directory 0` | 两个真实目录像素均正确 |
| `tail_ptr_past_eof.tif` | 第三次读取返回 0，next 置 0 | ERROR `Can not read TIFF directory count` ×2 | 真实目录正常 |
| `tail_ptr_into_pixels.tif` | 同上 | mmap：WARN 分配 15408 > 文件 268 + ERROR；stream：ERROR 短读 | 真实目录正常 |
| `unknown_tag_unknown_type.tif` | 目录读取成功 | ERROR（可恢复性质）`custom tag 65000 ... TIFF_SETGET_UNDEFINED ... not read` | 正常解码（`0xff3e3e3e..`） |
| `bigtiff_overflow_count.tif` | open 失败（两条 I/O 路径一致） | ERROR sanity + failed at offset 16 | 无 |
| `bigtiff_tail_past_eof.tif` | 第二次读取返回 0 | ERROR count/directory + `TIFFAdvanceDirectory ... Error fetching directory count` | dir0 正常（`0xff4d4d4d`） |

关键观察：

1. **mmap 与流式路径的错误文案不同但判定一致**；mmap 倾向“分配超文件大小”
   warning，流式倾向短读 error，所有样本两路径返回码相同。
2. 错误分两级：**结构级错误（count/截断/环）决定链能否继续；标签级错误只影响
   单个标签或单次解码**。环只产生 warning，其他结构损坏产生 error。
3. `tiffinfo -d` 作为独立工具复现了全部判定（`results/tiffinfo_run.log`），
   退出码在首 IFD 损坏时为 1。

---

## 3. 后续合法目录是否会复用脏状态

会，而且存在三种不同形态。用 `scripts/probe_state.c` 与
`probe_ignore_rc.c` 在三 IFD 链上实测（修复前）：

| 失败发生的位置 | 失败后 `tif_dir` 内容 | `tif_nextdiroff` | 客户端忽略返回码继续解码的后果 |
|---|---|---|---|
| **fetch 阶段**（count 超限/截断，`TIFFFetchDirectory` 返回 0） | **完整保留上一目录**的几何与条带数组（实测 4×4、nstrips=1） | 0 | `TIFFReadRGBAImage` 返回 1，**解码出的是上一目录的像素**（`0xff0c0c0c`），句柄呈现一个不属于任何真实 IFD 的“幽灵图像” |
| **解析阶段、count 保持正确**（未知类型等，在 Free/Default 之后 `goto bad`） | 已被 `TIFFFreeDirectory+TIFFDefaultDirectory` 重置为干净默认值（实测 4×0、nstrips=0） | 保留正确 next（268） | 几何为 0，解码被零尺寸检查拦住；合法 IFD2 仍可经 `TIFFSetDirectory(2)` 到达 |
| **解析阶段、count 被改小**（短目录） | 重置为默认值（4×0） | **垃圾值 196865**（从条目体内误读） | 解码被拦住，但链被污染：合法 IFD2 被永久孤立，随机访问也无法到达 |

根因（`tif_dirread.c:4371-4378` 修复前）：

```c
dircount = TIFFFetchDirectory(tif, nextdiroff, &dir, &tif->tif_nextdiroff);
if (!dircount) {
    TIFFErrorExtR(...);
    return 0;            // ← 直接返回，TIFFFreeDirectory() 在后面才执行
}
...
TIFFFreeDirectory(tif);  // 4424 行：fetch 失败时根本到不了
TIFFDefaultDirectory(tif);
```

即 fetch 失败时 `tif_diroff` 已指向坏偏移、`tif_nextdiroff` 可能被覆写，
而 `tif_dir` 还是上一目录的内容。注意 `tif_curdir` 的注释声称
“只在成功读取后更新”，但实测 fetch 失败后 `curdir` 停留在旧值（0），
与 `diroff=122` 自相矛盾，进一步说明句柄状态不一致。

补充：`TIFFReadCustomDirectory()`（`tif_dirread.c:5398` 一带）存在完全相同的
“fetch 失败即返回、Free/Default 在其后”模式，本次修复未覆盖，建议后续按同样
方式处理。

---

## 4. 最小修复与测试方案

### 4.1 已实施的代码修复（`libtiff/tif_dirread.c`）

**修复 A — 消除 fetch 失败后的脏状态泄漏（核心）**

在 `TIFFReadDirectory()` 的 fetch 失败分支中，返回前把目录重置为干净默认态：

```c
if (!dircount)
{
    TIFFErrorExtR(tif, module, "Failed to read directory at offset ...");
    TIFFFreeDirectory(tif);      /* 释放上一目录的全部堆数据与字段位 */
    TIFFDefaultDirectory(tif);   /* 几何/条带数组回到零默认值 */
    tif->tif_dir.td_iswrittentofile = TRUE;
    return 0;
}
```

- 安全性：`CleanupField` 宏释放后立即置空（`tif_dir.c:1632`），
  `TIFFClose()` 再次调用 `TIFFFreeDirectory()` 不会双重释放；全量
  157 项 ctest 无回归。
- 效果实测：同样的 overflow 样本，失败后几何由 `4×4` 变为 `0×0`，
  忽略返回码再解码会被零尺寸检查拒绝，幽灵图像消失；`SetDirectory(0)`
  仍可回到真实目录。

**修复 B — BigTIFF 64→16 位计数截断显式拦截（mmap 与流式两处）**

在两处 `dircount16 = (uint16_t)dircount64` 之前增加：

```c
if (dircount64 > 0xFFFF) {
    TIFFErrorExtR(tif, module,
        "Sanity check on directory count failed, count %" PRIu64
        " exceeds 65535", dircount64);
    return 0;
}
```

当前 4096 上限使该分支不可达，但它把“不能截断”从隐式约定变成显式不变量，
防止将来上调 sanity 上限时静默截断复活。

**修复 C — 流式路径对齐 0 计数检查**

流式路径在截断检查后补充 `dircount16 == 0` 拒绝（mmap 路径原本已有），
使两条 I/O 路径对零条目目录行为一致。

### 4.2 建议但本次未做的修复（记录在案）

1. **短目录的 next 指针污染**：`TIFFFetchDirectory` 按伪造短计数定位 next 指针，
   会从条目体内读出垃圾偏移。最小缓解是在读取 next 指针后校验其位于
   `diroff + 2 + real_count*12` 预期位置，或要求 next 偏移为 0 或 ≥ IFD 尾端且
   ≤ 文件大小。该项会改变现有宽松容忍行为，需要单独评审。
2. `TIFFReadCustomDirectory()` 的同类 fetch 失败脏状态（见第 3 节末）。
3. 可考虑把 `tif_curdir` 在 fetch 失败时显式置为
   `TIFF_NON_EXISTENT_DIR_NUMBER`，与 `TIFFSetDirectory()` 的失败约定
   （`tif_dir.c:2179`）保持一致。

### 4.3 测试方案（已落地：`test/test_corrupt_ifd.c`，注册进 CTest）

新增 `test_corrupt_ifd`（`test/CMakeLists.txt` 已注册，全套 157 项测试通过），
用 libtiff 自身 API 生成三 IFD 的 Classic/BigTIFF 文件并定点二进制覆写，
每个场景同时验证错误返回与句柄状态：

1. **越界读/脏状态**：中间 IFD 计数改 5000 → 期望第二次 `TIFFReadDirectory`
   返回 0，且失败后 `IMAGEWIDTH/IMAGELENGTH` 读出 0（修复前为泄漏的 4×4，
   已用 `git stash` 验证该测试在无修复时确实失败：
   `stale geometry leaked after failed read (4x4)`）；随后
   `SetDirectory(0)` 必须成功且宽度恢复为 4。
2. **整数截断/超大计数**：同一损坏分别以 `"r"` 与 `"rm"` 打开，
   两条路径都必须干净拒绝；Classic 与 BigTIFF 各跑一遍。
3. **目录循环**：尾 IFD 的 next 指针改回首 IFD → 期望前三个目录读取成功、
   第四次读取被环检测拒绝；`SetDirectory(0..2)` 全部成功、
   `SetDirectory(3)` 必须失败，保证真实目录不被环破坏。

复现命令：

```bash
# 构建（ASan/UBSan）
cmake -S . -B asan-build -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build asan-build -j
# 回归
(cd asan-build && ctest --output-on-failure)          # 157/157 通过
(cd asan-build && ./test/test_corrupt_ifd)            # All corrupt IFD tests passed.
# 损坏样本复现
python3 -I corrupt-ifd-work/scripts/gen_samples.py corrupt-ifd-work/samples
python3 -I corrupt-ifd-work/scripts/gen_extra.py   corrupt-ifd-work/samples
bash corrupt-ifd-work/scripts/run_tests.sh
```

### 4.4 结论

- 现有库对**溢出计数、未知类型、越界偏移、IFD 环、截断文件**的主路径防护是
  健全的：24 个畸形样本无内存安全问题，环检测与 4096 sanity 上限有效。
- 确认的真实缺陷是 **fetch 阶段失败后 `tif_dir` 残留上一目录状态**，
  可被忽略返回码的客户端用来解码出错误的“幽灵图像”；已由修复 A 解决并由
  回归测试锁定。
- 短目录导致的 next 指针垃圾值是剩余的健壮性缺口（不导致内存不安全，
  但会孤立后续合法目录），已给出最小修复思路，建议单独评审后合入。
