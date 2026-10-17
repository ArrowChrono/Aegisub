# 字体规范化安全

- 自动规范化名称或变体，必须在完整 staged 结果上证明所有受影响文本的有效字体选择不变。单项建议可应用、family ID 相同，都不足以证明整体安全。
- 必须保持匹配物理实体、role、实际字重、实际斜体和 backend。重放样式继承、family 变化、reset、显式粗／斜体及 drawing 后文本；全部选中改动一起验证后才能修改原对象，组合计划失败时不得部分提交。
- 原始请求名、拟用别名及相关 `@` 形式分别 live probe。不能先把两种名称规范化成同一本地化 family 再比较，否则会抹掉需要检查的差异。
- 候选副本保留对象 ID。计划拥有字体目录快照；live resolver／profile 缓存仅限一次操作，不得超过借用 resolver 的生命周期，也不能掩盖后续系统字体变化。
- 无法证明保持不变时，使用 `font_state_preservation_unproven`，只报告建议、不自动应用。字体相关 transform、相对字号和未解析的样式／reset 应保守处理，不能猜测等价。
- GUI 单独重建名称计划，也必须经过同一组合安全预检；切换目标命名语言时不能退回旧的 metadata-only 路径。
- 既有 CLI `normalize` 和名字计划 C API 仍为 report-only，不写字幕。

源码入口：[共享审计实现](../../src/font_variant_audit.cpp)、[组合计划](../../src/font_portability_audit.cpp)、[GUI 名称重建](../../src/dialog_font_portability_audit.cpp)。
