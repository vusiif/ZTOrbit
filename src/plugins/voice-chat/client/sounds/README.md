# 语音提示音资产

本目录的两段提示音由 [generate.py](generate.py) 确定性合成：

- `join.wav`：660 Hz → 880 Hz（上行双音）。
- `leave.wav`：880 Hz → 660 Hz（下行双音）。
- 每段约 160 ms，48 kHz、单声道、PCM16；每个音段起止做 5 ms 淡入淡出。

重新生成（需要 Python 3）：

```powershell
python src/plugins/voice-chat/client/sounds/generate.py
```

资产授权：合成参数和生成脚本由本项目贡献，按 CC0 1.0 公共领域贡献（Public
Domain Dedication）提供；不包含第三方录音、采样或需要署名授权的素材。音色只由
正弦波与线性包络构成，可由脚本完整复现，生成结果不依赖系统音效目录或网络。
