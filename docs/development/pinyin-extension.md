# AC101S 扩展拼音词库

固件发布包中提供经过验证的 `mandarin-extension.bin`、JSON 清单和 `install_pinyin_extension.py`。请在解压目录使用 JSON 中的 `sha256`，并填入目标 AC101S 的实际串口和设备 MAC。安装脚本需要提供 `esptool` 5 和 `pyserial` 的 Python 环境。在源码目录使用脚本时，脚本路径为 `tools/install_pinyin_extension.py`。

```powershell
$pinyinHash = (Get-Content mandarin-extension.json -Raw | ConvertFrom-Json).sha256
$aetherPort = 'COM3' # 改为实际端口
$aetherMac = '00:00:00:00:00:00' # 改为设备的实际 MAC
python install_pinyin_extension.py check mandarin-extension.bin --port $aetherPort --mac $aetherMac --sha256 $pinyinHash
python install_pinyin_extension.py install mandarin-extension.bin --port $aetherPort --mac $aetherMac --sha256 $pinyinHash
```

扩展词库使用 AC101S 64 MiB 闪存 `0x00900000` 起的 3 MiB 区域。`check` 只读取和校验，`install` 在同样的检查通过后写入词库并重新校验设备中的数据。脚本识别已安装的扩展版本并跳过相同数据，设备的分区表、应用、NVS 和 TF 卡保持原样。

源码中的 `tools/generate_pinyin_extension.py` 可从注明的上游词典重新生成扩展。生成器根据当前内置词库去重，内置短句更新后，重新生成的词库哈希可能与发布包中的实测版本不同。
