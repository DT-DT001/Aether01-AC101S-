# ESP32-S31 蓝牙连接槽故障证据

2026-11-03，原固件可发现耳机，但发起 A2DP 连接失败。本次只读分析使用对应发布产物，未用后续重编译 ELF 解释旧地址：

- 固件：`build/releases/unified-volume-radio/aether01.bin`，SHA256 `5516ecd130d468e79a3ff83d29e867697a5e1efed34dca77b79e13bea3ccf59e`。
- 配套 ELF：`build/releases/unified-volume-radio/aether01.elf`，SHA256 `0b01907019e87dd44a89efcc0e2cff46f8b789596d75b366f77f35741c1d44fe`；启动日志也显示 ELF `0b0190701...`。`5516` 是 BIN 标识。
- 复现日志：`tmp/headphone-connect-before-fix.log`；旧配置 `CONFIG_BT_CTRL_BR_EDR_MAX_ACL_CONN=1`、`MAX_SYNC_CONN=0`，Classic-only。

## 最先失败的位置

日志依次为：

```text
OLM: hci_create_conn_handler alloc conn failed!
BT_HCI: opcode=0x0405, status=09: Conn Limit Exceeded
BT_SDP: SDP - Rcvd conn cnf with error: 0x9
BT_BTC: BTA_AV_OPEN_EVT::FAILED status: 2
```

失败发生在 HCI Create Connection，尚未进入 PIN/SSP 配对。SDP/A2DP 的错误是后续传播，不能单凭最终 `OPEN_FAILED` 判断密码或 SBC 编码问题。

对应 ELF 最短反汇编证据：

```text
400c8636  jal  r_olm_conn_alloc        ; 返回非零时打印 alloc conn failed

400c1b26  jal  r_olm_conn_find_unused
400c1b2c  li   s1,9
400c1b2e  beqz a0,400c1b66            ; 没有空槽：返回 09
```

`r_olm_conn_alloc` 在这一分支没有申请堆内存。`r_olm_conn_find_unused` 遍历 OLM 配置中的槽数量，检查每个预分配槽偏移 32 的标志 bit0；所有槽都被占用时返回空指针。`r_olm_env_init` 在初始化时分配 `72 + 56 × 槽数` 字节的基本环境及槽数组，另有可选同步连接数据。初始化会逐槽清除 used 位。

**09 槽耗尽与 07 内存失败是两个不同路径。** 同一 handler 的另一路径先申请 14 字节私有池内存，失败会返回 07，并打印不同的 `malloc create_conn_param failed`：

```text
400c84dc  li   a0,14
400c84de  ...  r_orca_mempool_malloc
400c84e8  bnez a0,...                 ; 成功才继续
400c84f2  li   s0,7                   ; 私有池申请失败：07
```

因此，这次具体的 09 不能解释为“当场向全局 DMA heap 分配连接对象失败”。当时 DMA 空闲约 6.9KiB、最大块约 3.8–4.3KiB，仍然说明并发余量很紧，但不是上述失败分支的直接证据。

## 源码与配置依据

本机 IDF 根目录为 `C:/APP/ESP/.espressif/v6.1/esp-idf`：

- `components/bt/porting_btdm/controller/bredr/Kconfig.in:1`：最大 ACL 数默认 **2**，合法范围 **1–7**。不能据此宣称 S31 规定至少两条连接。
- `components/bt/include/esp32s31/include/esp_bt.h:190`：`max_acl_conn` 字段；默认配置宏把 `UC_BR_EDR_MAX_ACL_CONN` 传入该字段。匹配 ELF 的初始化代码把该数量复制到 OLM 配置。
- `components/bt/porting_btdm/controller/bredr/src/bredr.c:875`：私有池为 `1024 × (max_acl_conn + 2)`，ACL1→2 会使该池从 3KiB 增至 4KiB，并增加槽及其他控制器结构；不能只计 56 字节作为总成本。
- `components/bt/porting_btdm/controller/btdm_common/src/btdm_osal_freertos.c:1152`：控制器 INTERNAL 请求使用 `INTERNAL | 8BIT | DMA`，不因 Bluedroid 的 PSRAM-first 设置而迁往 PSRAM。
- `components/bt/common/osi/include/osi/allocator.h:39`：Bluedroid 普通 `osi_malloc` 受 PSRAM-first 控制。A2DP 的 1000 字节 SDP 数据库走该分配器，不能一概视为新增内部 DMA 需求。
- `examples/bluetooth/bluedroid/classic_bt/a2dp_source/main/main.c:333`：官方 source 例程使用 `ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE`，由 source 主动连接所选设备。

可用现有工具复查，不需安装 IDF：

```powershell
& 'C:/Espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin/riscv32-esp-elf-objdump.exe' `
  -d -C --disassemble=r_olm_conn_alloc build/releases/unified-volume-radio/aether01.elf
```

## 修复假设与实机验收边界

恢复 SDK 默认 ACL2 是合理的兼容/诊断起点；source 改成 NON_CONNECTABLE，可避免其他设备先发起入向 ACL 占用唯一槽。两项都仍需真实耳机连接验证，不能把增加槽数当作已证明的根因修复，也不能声称 A2DP 与 AVRCP 必须各占一条 ACL。

现有日志无法证明具体是谁占了槽：可能是未选设备的入向连接、尚未释放的失败连接或其他控制器状态。需要补记 ACL 建立/断开事件中的地址、状态、handle、reason，以及 A2DP/认证结果；冷启动后重现一次，确认旧槽不会随失败重试累计。不要记录 PIN、密码或链路密钥。

验收应覆盖耳机首次配对、断开重连、反复失败重试、Wi-Fi 开关两种条件，以及音乐播放后的实际耳机出声。同步记录内部/DMA heap 和连接事件；若出现真正的内存分配失败，再按该分配路径调整预算。本文写入时，ACL2/NON_CONNECTABLE 的实机连接结果尚待确认。

## 本轮实机结果

修正镜像`038387d9...`（配套ELF `a81c49f46...`）已在测试设备上完整烧录并通过三段哈希校验。实际初始化日志确认Classic mode2、ACL2、同步连接0；初始化前内部DMA空闲93,027字节/最大块63,488，初始化后63,611/61,440。

用户确认UGREEN“已连接且有声音”，再确认“重连正常，持续有声”（约一分钟）。180秒被动观察`tmp/headphone-link-acceptance.log`记录第一次ACL及认证均status0，A2DP CONNECTED，SBC44.1kHz joint stereo；主动断开后，原handle0x0801被释放，第二次连接成功复用同一handle，AAC-LC音乐从external输出继续推进。活跃播放时一次DMA采样为37,811字节空闲/最大块23,552。未再次捕获HCI09、SD DMA失败或panic；保留原始SDK省电协商等E/W，不声称零错误日志。

该结果验证了这组修正对本耳机的首次连接和一次重连有效，未证明旧固件唯一槽位的具体占用者，也不等同全部耳机、Wi-Fi共存或长时间稳定性已经通过。配套产物与日志索引见`build/headphone-link-manifest.json`。
