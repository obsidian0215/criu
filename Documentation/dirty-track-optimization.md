# Dirty-Track 迭代预转储优化技术文档（与当前实现一致 · 细化版）

本文档对齐当前实现（`criu/mem.c`、`criu/dirty-map.c`、`criu/include/dirty-map.h`、`scripts/dirty-track/checkpoint_run_impl.sh`），并补全：字段语义、权重来源与调整方式、完整决策流程、可执行伪代码/公式、以及基于 dirtymap + deferred_list 的动态停止策略说明。

## 0. 摘要

- **dirty-map 为权威来源**：在 `--use-dirty-map` 模式下，**dirtymap 缺失条目视为“未写”**
- **diffmap 仅使用 heat + heat_trend**：$heat = \frac{write\_count}{track\_s}$，$heat\_trend = heat_{cur} - heat_{prev}$（仅最新两份 dirtymap 计算）。
- **deferred_list 为跨轮核心状态**：记录延迟页地址与连续 defer 轮次（count），并通过 per-class FIFO 队列实施软预算回补与保护期。
- **首轮/首进程激进 defer（非全量）**：每进程首次进入 predump（仅 1 份 dirtymap）时使用单 dirtymap 特征进行激进 defer，cap 约 **0.90**（再乘 PID 热度因子并 clamp 到 0.95）。
- **无固定 drain/stop 轮次**：改为“冷却触发 + 逐出倒计时（TTL）”的 per-page 逐出逻辑。
- **决策模型三态（默认 online）**：`legacy`（score→sigmoid）、`online`（score→在线校准，**默认**）、`offline`（logistic skip）。
- **在线校准含页类型修正**：$p=\sigma((scale+class\_scale)\cdot score + (bias+class\_bias))$，由 deferred 反馈在线更新。
- **p_thr 二次修正（mem.c）**：VMA 类型系数 + 趋势/dirty_freq 风险 + defer 疲劳 + 上一轮准确率（R>1）。
- **脚本层自适应停止**：仅基于 dirtymap/deferred 信号（DM 专属），避免“表面收敛但 deferred 发散”。
- **固定停止策略**：pages-based 的固定规则（balanced 值），与 adaptive 解耦。

## 1. 目标、边界与术语

### 1.1 目标
- **减少冗余页传输**：降低 predump/dump 页数与耗时。
- **避免尾轮爆发**：软预算回补 + 延迟冷却策略，降低 final dump 峰值。
- **保证正确性**：最终 dump 必须包含所有 deferred 页。

### 1.2 术语
- **Round**：预转储轮次，`round = parent_chain_len + 1`。
- **dirtymap / diffmap**：LKM 输出的 `*.dirtymap` 与由相邻 dirtymap 合并的 diffmap。
- **deferred_list**：跨轮持久化的“被延迟页集合”，每页携带连续延迟轮数 count。
- **urgent_force**：软预算强制回补集合（需立即 dump）。
- **parent reliability**：仅检查“直接父层”，`PE_DEFERRED` 视为不可靠。

## 2. 架构与数据流

```
┌────────────────────────────────────────────────────────────────────────┐
│                               用户态 CRIU                               │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │ mem.c: generate_iovs_with_dirty_map()                             │  │
│  │  - parent_has_reliable_page()                                     │  │
│  │  - deferred_list / urgent_force / PP_HOLE_*                       │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                    ↑                                     │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │ dirty-map.c: load/merge diffmap + 模型计算 + 阈值/队列            │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
                                                                         │
┌────────────────────────────────────────────────────────────────────────┐
│                          内核态 dirty-track LKM                          │
│  PTE 追踪 → /tmp/dirtymap/<ctid>/<pid>-<ts>.dirtymap                    │
└────────────────────────────────────────────────────────────────────────┘
```

核心流程：
1. **Stop dirty-track** 生成 dirtymap；
2. **merge** 最新/次新 dirtymap → diffmap；
3. **更新 page_history** 与全局 heat 统计；
4. **决策**：DEFER / PP_HOLE_PARENT / DUMP；
5. **soft-budget enforcement**：按热度/类别回补 deferred → urgent_force。

## 3. 产物与文件格式（磁盘状态）

### 3.1 `*.dirtymap`
- 路径：`<dirtymap_dir>/<pid>-<timestamp>.dirtymap`
- 文件结构：
    - Header：`dirtymap_header_t`
        - `track_duration_ns`：追踪窗口长度（纳秒）
    - Body：`dirty_map` 数组
        - `address`（页对齐地址）
        - `write_count`（窗口内写次数）
- 计算：
    - $track\_s = track\_duration\_ns / 10^9$
    - $heat = write\_count / track\_s$

### 3.2 `timestamp_list.<pid>`
- 记录 dirtymap 时间戳序列，用于定位 latest/less-latest dirtymap。

### 3.3 `deferred_list.<pid>`
- **新格式**（推荐）：`deferred_page_t` 数组
    - `address`（页地址）
    - `count`（连续延迟轮数；0 视为 1）
- **旧格式**：仅地址数组（默认 count=1）
- CRIU 依据文件大小自动识别新/旧格式。

### 3.4 `threshold.<pid>`
- 两个 `float`：`heat_threshold` 与 `trend_threshold`，跨轮持久化。

### 3.5 其他
- `warm_list.<pid>`：**已移除**（不再生成/读取）
- `prediction_metrics.*`：可选（脚本会复制，来源可能是外部工具）
- `convergence_metrics.*`：可选（脚本会复制，用于外部分析）

## 4. 核心数据结构与字段语义

### 4.1 `dirty_log` 关键字段
- **diffmap 相关**
    - `diffmap`, `diffmap_size`
    - `ldm_header.track_duration_ns`
    - `min_heat`: $1/track\_s$，用于判断“已降温”
    - `global_mean_heat / global_median_heat / global_p75_heat / global_p90_heat / global_max_heat`
    - `stats_collected`：全局热度统计是否已就绪
- **deferred 相关**
    - `deferred_list` / `deferred_size`
    - `deferred_ever` / `deferred_unique_total` / `deferred_sent_after_defer`
    - `deferred_protected_until`: `protected_until = round + MIN_DEFER_COOLDOWN_ROUNDS - 1`
    - `deferred_evict_ttl`: per-page 逐出倒计时（cooling 触发）
    - `deferred_queues[PAGE_CLASS]`: per-class FIFO
    - `deferred_heat_sum`: deferred 页 heat 总和
    - `prev_dirty_set`: 上一轮 dirty 页集合（用于扩展准确率）
    - `urgent_force`: 本轮强制回补集合
    - `current_round`: 供阈值/软预算/准确率修正使用
- **统计与自适应**
    - `predicted_total/hit/miss`: deferred → cooled 预测准确率
    - `predicted_*_nondefer`: 上一轮未 defer 页在本轮的正确性
    - `predicted_*_all`: defer + nondefer 综合准确率
    - `adaptive_p_base` / `adaptive_soft_ratio`
    - `adaptive_max_defer[class]`
- **首轮控制**
    - `first_predump_done` / `first_predump_round`: 每进程首轮激进 defer 开关
- **历史趋势**
    - `page_history_map`
    - `score_ema`（EMA 平滑）
    - `mean_heat / variance_heat / dirty_freq / is_declining / is_stable`

### 4.2 `deferred_page_t`（文件与内存一致）
- `address`：页地址
- `count`：连续 defer 次数（每次 defer 增 1）

### 4.3 `dirty_map` / `dirtymap_header_t`
- `dirtymap_header_t.track_duration_ns`：追踪窗口长度（纳秒）
- `dirty_map.address`：页对齐地址
- `dirty_map.write_count`：窗口内写次数

### 4.4 `dirty_diffmap`
- `address`：页对齐地址
- `heat`：$write\_count/track\_s$
- `heat_trend`：$heat_{cur} - heat_{prev}$

### 4.5 `page_history_t`
- `heat[MAX_HISTORY_ROUNDS]` / `history_count` / `last_round`
- `mean_heat` / `variance_heat` / `dirty_freq`
- `is_declining` / `is_stable`
- `score_ema`

### 4.6 `decision_model_t`（运行时）
- legacy 权重：`W0/W_HEAT/W_TREND/.../CLASS_BIAS`
- online 校准：`online_bias/online_scale/online_class_bias/online_class_scale`
- offline 模型：`logistic_weights/intercept/threshold`

## 5. 热度与阈值计算

### 5.1 heat / heat_trend / writes_est
$$
track\_s = \frac{track\_duration\_ns}{10^9},\quad
heat = \frac{write\_count}{track\_s},\quad
heat\_trend = heat_{cur} - heat_{prev},\quad
writes\_est = heat \cdot track\_s
$$

### 5.2 diffmap 合并规则（最新/次新）
- **仅在 latest**：$heat = heat_{cur}$，$heat\_trend = heat_{cur}$（等价于 $heat_{prev}=0$）
- **仅在 less-latest**：$heat = 0$，$heat\_trend = -heat_{prev}$
- **两者同时存在**：$heat = heat_{cur}$，$heat\_trend = heat_{cur} - heat_{prev}$

### 5.3 min_heat（“至少写 1 次”阈值）
$$
min\_heat = \frac{1}{track\_s}
$$
在 `update_prediction_from_deferred()` 中，当页面 heat $\le$ min_heat 或 diffmap 中不存在时，视为“已冷却”。

### 5.4 动态阈值（多级）
- Round1 收集全局统计（仅非零 heat）：`global_mean_heat / median / p75 / p90`
- 阈值更新：
    - `threshold_mid = 0.5 * median_heat`
    - `threshold_low = threshold_mid * 0.3`
    - `threshold_high = threshold_mid * 2.5`
- aggression 随轮次降低：R1=1.0、R2=0.95、R3=0.90、R4=0.85、R5+=0.80。

## 6. 页面分类与访问模式

### 6.1 页面分类
- **PAGE_COLD**：$heat = 0$（真正无活动的冷页）
- **PAGE_FREEZING**：$heat < threshold\_low$ 且 $\frac{heat\_trend}{heat} \le -0.6$（强降温，倾向直接回补）
- **PAGE_WARM**：低热但非 freezing（低热稳定页，延迟更谨慎）
- **PAGE_HOT**：$heat \ge threshold\_mid$（合并原 BURNING 行为）

### 6.2 访问模式
- **STABLE**：$heat > threshold\_mid$ 且 $|trend| < 0.15 \cdot heat$
- **BURST**：$trend > 1.5 \cdot heat$
- **DECLINING**：$trend < 0$ 且 $|trend| > 0.5 \cdot heat$
- **PERIODIC**：预留（未实现）

### 6.3 分类在算法中的意义
- **score 偏置**：`CLASS_BIAS` 直接修正 $score$（FREEZING/COLD 更保守，HOT 更易 defer）。
- **p_thr 类别系数**：`class_factor` 让 FREEZING/COLD 更难 defer、HOT 更容易 defer。
- **max_defer(class)**：hot/warm/cold 具有不同的最大连续 defer 轮次。
- **软预算回补顺序**：per-class FIFO 队列顺序为 COLD → FREEZING → WARM → HOT。
- **逐出 TTL 调整**：HOT 类别在 cooling 时 TTL 会略放宽（+1）。

## 7. 历史趋势与稳定性

- `page_history_t` 记录最近 `MAX_HISTORY_ROUNDS` 轮 heat；若 round 不连续，会用 `heat=0` 进行补零近似（“全历史”）。
    - $mean = \frac{1}{n}\sum heat_i$
    - $variance = \frac{1}{n}\sum (heat_i - mean)^2$
    - $cv = \sqrt{variance}/mean$
    - $dirty\_freq = \frac{count(heat>0)}{n}$：衡量“生命周期”，用于区分“稀疏脏页/长期脏页”。
- 稳定判定：$cv < 0.3$；下降判定：最近 3 次 heat 非增。
- 在决策模型中：
    - $dirty\_freq$ 过低 → **transient penalty**（避免把偶发脏页 defer）
    - $dirty\_freq$ 高且 $cv$ 低且不降温 → **persistent penalty**（降低反复 defer 的误判）

`score_ema` 用于平滑决策分值，降低短期噪声。

## 8. 决策模型（legacy / online / offline）

### 8.1 特征提取（dirtymap / diffmap / history / deferred）
- **dirtymap/diffmap 基础量**：
    - $track\_s = \frac{track\_duration\_ns}{10^9}$
    - $heat = \frac{write\_count}{track\_s}$
    - $heat\_trend = heat_{cur} - heat_{prev}$（仅使用 latest 与 less-latest 两份 dirtymap）
    - $writes\_est = heat \cdot track\_s$
- **归一化与置信度**：
    - $heat\_norm = \frac{heat}{threshold\_mid + \epsilon}$
    - $trend\_norm = \frac{heat\_trend}{heat + \epsilon}$
    - $confidence = \frac{writes\_est}{writes\_est + CONF\_K}$
- **访问模式**（基于 diffmap 即时特征）：`PATTERN_BURST / PATTERN_DECLINING / PATTERN_STABLE`
- **历史统计**（`page_history_t` 最近 $MAX\_HISTORY\_ROUNDS$ 轮）：
    - `history_count / mean_heat / variance_heat / dirty_freq / is_declining / is_stable / score_ema`
- **deferred 特征**：
    - `deferred_count`
    - $deferred\_risk = \frac{deferred\_heat\_sum}{global\_mean\_heat \cdot diffmap\_size + \epsilon}$
- **类别特征**：`page_class` + `CLASS_BIAS`；`is_warm` 由“稳定低热的 PAGE_WARM”派生（warm_list 已移除）。

### 8.2 Legacy 线性 score + sigmoid（作为基础分数）
分值（实现一致）：

$$
\begin{aligned}
w_{dirty} &= W_{DIRTY\_FREQ} \cdot \begin{cases}
1, & predicted\_total \le 100 \\
\min\left(1, \frac{pred\_rate}{0.65}\right), & predicted\_total > 100
\end{cases} \\
score = &\; W_0 + W_{HEAT} \cdot heat\_norm \cdot (0.5 + 0.5 \cdot confidence) \\
& + W_{TREND} \cdot trend\_norm \cdot confidence + W_{DEFER} \cdot deferred\_count \\
& + W_{BURST} \cdot pat\_{burst} + W_{WARM} \cdot is\_warm \\
& + w_{dirty} \cdot dirty\_freq \\
& - P_{decl} \cdot pat\_{decl} - H_{decl} \cdot hist\_{decl} + CLASS\_BIAS[page\_class] \\
& + hist\_boost - DEFER\_RISK\_COEF \cdot deferred\_risk \\
& - transient\_penalty - persistent\_penalty
\end{aligned}
$$

其中：
- $hist\_boost$：长期低热但持续脏的保守上调（0.2~0.8 上限）。
- $transient\_penalty$：$dirty\_freq$ 过低、$cv$ 过高、或 BURST 时的惩罚（避免偶发页 defer）。
- $persistent\_penalty$：$dirty\_freq$ 高且 $cv$ 低且不降温时的惩罚（降低反复 defer）。
- $score$ 被截断到 $[-SCORE\_CLAMP, SCORE\_CLAMP]$，并通过 $score\_ema$ 平滑。

最终概率：$p_{next} = \sigma(score)$。

### 8.3 Online 校准（默认，无需环境变量）
在线模型保留 legacy 的 $score$，只做 **概率校准**，并额外引入页类型级别的校准项：

$$
p_{online} = \sigma\big((scale + class\_scale_{c}) \cdot score + (bias + class\_bias_{c})\big)
$$

**在线更新**来自 deferred 反馈（上一轮被 defer 的页）：
- 目标 $y=1$ 表示“已降温”（diffmap 缺失或 $heat \le min\_heat$），否则 $y=0$。
- 误差 $err = p - y$。
- 更新（含 $L2$ 正则）：
$$
\begin{aligned}
bias &\leftarrow bias - lr\,(err + l2\cdot bias) \\
scale &\leftarrow scale - lr\,(err\cdot score + l2\cdot scale) \\
class\_bias_c &\leftarrow class\_bias_c - lr\,(err + l2\cdot class\_bias_c) \\
class\_scale_c &\leftarrow class\_scale_c - lr\,(err\cdot score + l2\cdot class\_scale_c)
\end{aligned}
$$
并将 `bias/scale` 以及 per-class 项限制在 $[-10, 10]$。

默认参数：`CRIU_ONLINE_LR=0.05`，`CRIU_ONLINE_L2=1e-4`。

> 需要切换时可设置：`CRIU_PREDICT_MODEL=legacy|offline`。

### 8.4 Offline logistic skip（`CRIU_PREDICT_MODEL=offline`）
- 启用：`CRIU_SKIP_MODEL_FILE=<path>`
- 特征顺序（与 `decisions.csv` 对齐）：
    0 heat, 1 heat_trend, 2 writes_est, 3 track_s, 4 hist_count, 5 hist_mean,
    6 hist_variance, 7 deferred, 8 old_p, 9 score, 10 pthr, 11 round
- $p_{logit} = \sigma(w \cdot x + b)$；`best_threshold` 作为 `decision_p_threshold` 最低值。
- 可用 `CRIU_SKIP_MODEL_MIN_THRESHOLD` 提高保守性。

### 8.5 p_thr 与自适应
- 基础阈值：
    $$p_{thr} = adaptive\_p\_base \cdot factor(deferred\_risk) \cdot class\_factor \cdot round\_factor \cdot hot\_factor$$
    并受 $[0.05, 0.95]$ 约束。
- `adaptive_p_base` 基于 deferred 命中率自适应，目标 0.80。
- `adaptive_soft_ratio` 依据 $\frac{deferred\_heat\_sum}{global\_mean\_heat \cdot diffmap\_size}$ 自动收敛。
- **mem.c 二次修正**（R>1）：
    - VMA 类型系数（stack/共享/文件映射更保守）
    - 趋势/dirty\_freq 风险
    - defer 疲劳（deferred\_count 高且不降温）
    - 上一轮命中率低时整体保守化

## 9. Deferred 机制与软预算

### 9.1 deferred_list 语义
- 每次 defer：
    - `count++`
    - `protected_until = round + MIN_DEFER_COOLDOWN_ROUNDS - 1`
- `MIN_DEFER_COUNT_FOR_ENFORCE = 2`：低于该值不进入 soft-budget 回补。
 - **冷却逐出（TTL）**：当页冷却（含 cold）时启动倒计时；倒计时到 0 则从 deferred_list 逐出并回补。升温会取消倒计时。

### 9.2 per-class FIFO 队列
- 类别顺序：COLD → FREEZING → WARM → HOT
- 队列用于从“风险最低”的类优先回收。

### 9.3 soft-budget enforcement
- 预算热度：
    $budget = global\_mean\_heat \cdot diffmap\_size \cdot adaptive\_soft\_ratio$
- 若 `deferred_heat_sum` 超过预算，按 per-class FIFO 回补：
    - 低热优先
    - 尊重 `protected_until`
    - early round 更保守保留 HOT/WARM

### 9.4 deferred 正确性统计
- `deferred_ever`：所有“曾被延迟”的地址
- `deferred_sent_after_defer`：最终仍被发送的子集
- `final_decision_accuracy = 1 - sent_after_defer / deferred_unique_total`

## 10. 轮次与父链可靠性

- `round = parent_chain_len + 1`
- 只检查直接父层：
    - `PE_DEFERRED` → 不可靠
    - `PE_PRESENT` → 可靠
    - gap / `PE_PARENT` → 允许

## 11. 逐页决策流程（实现级）

### 11.1 轮次与关键常量
- `defer_cap_ratio = 0.60 - 0.05*(round-1)`，下限 0.30
- **首轮/首进程激进**：若为进程首个 pre-dump 且仅有一份 dirtymap，则 cap≈0.90（再乘 PID 热度因子并 clamp 到 0.95）。
- `defer_cap_ratio` 还会乘 `PID hotness factor`（更热 → cap 更高）。
- `allow_defer = (deferred_size / diffmap_size < defer_cap_ratio)`
- `DEFER_EVICT_TTL_BASE=2`，`DEFER_EVICT_TTL_MAX=5`：冷却页逐出倒计时。

### 11.2 预转储决策流程
> `dirty_now` 基于 diffmap：`writes_est > 0` 视为“当前脏”；diffmap 缺失则视为“未写”。
1. **urgent_force** 命中 → 立即 DUMP，并从 deferred_list 移除
2. 若 `is_deferred && !dirty_now`：
    - 保护期内 → DEFER
    - **降温** → 启动/递减逐出倒计时（TTL），TTL 到 0 则 DUMP
    - **升温** → 取消倒计时，继续 DEFER
    - 其他（稳定）→ DEFER
3. **首个 pre-dump（单 dirtymap）**：若 `dirty_now` 且 `heat >= threshold_low` → DEFER（cap≈0.90，受 PID 热度因子与 clamp 约束）
4. `dirty_now == true`：
    - 计算 `p_next` 与 `p_thr`
    - `p_thr` 进一步按 **VMA 类型**、**趋势/dirty_freq 风险**、**defer 疲劳**、**上一轮准确率**（R>1）修正
    - `p_next >= p_thr` 且 `deferred_count < max_defer[class]` 且 `allow_defer` → DEFER
    - 否则 DUMP
5. `dirty_now == false`：
    - parent 可靠 → `PP_HOLE_PARENT`
    - 否则 DUMP

### 11.3 最终 dump
- 所有 deferred 页面必须 **FORCE_DUMP**

### 11.4 伪代码（实现语义）
```pseudo
if pre_dump:
    if urgent_force(v): DUMP (and clear deferred)
    else if deferred(v) and !dirty_now:
             if protected_until: DEFER
             else if warming: cancel TTL; DEFER
             else if cooling: TTL-- ; if TTL<=0 then DUMP else DEFER
             else DEFER
    else if first_predump and dirty_now and heat >= threshold_low and allow_defer: DEFER
    else if dirty_now:
             p_next = f(dhm, history, deferred)
             p_thr  = g(class, budget, round, hotness)
             p_thr *= vma_factor * risk_adj(trend, dirty_freq, defer_fatigue, acc)
             allow_defer_local = allow_defer && deferred_count < max_defer[class]
             if p_next >= p_thr and allow_defer_local: DEFER
             else DUMP
    else:
             if parent_reliable: PP_HOLE_PARENT
             else DUMP
else:
    if deferred(v): FORCE_DUMP
```

### 11.5 LaTeX 算法（更完整）
$$
\begin{aligned}
&\textbf{Input: } dhm,\; round,\; deferred\_count,\; parent\_ok,\; dirty\_now \\
&\textbf{If } pre\_dump: \\
&\quad \text{if } urgent\_force \rightarrow DUMP \\
&\quad \text{else if } deferred \wedge \neg dirty\_now: \\
&\qquad \text{if protected \rightarrow DEFER} \\
&\qquad \text{else if warming \rightarrow DEFER} \\
&\qquad \text{else if cooling: } TTL\leftarrow TTL-1; (TTL\le 0 ? DUMP : DEFER) \\
&\qquad \text{else } DEFER \\
&\quad \text{else if } first\_predump \wedge dirty\_now \wedge heat\ge threshold\_low \wedge allow\_defer \rightarrow DEFER \\
&\quad \text{else if } dirty\_now: \\
&\qquad p_{next} \leftarrow f(dhm,\;history,\;defer) \\
&\qquad p_{thr} \leftarrow g(class,\;budget,\;round,\;hotness) \\
&\qquad p_{thr} \leftarrow p_{thr} \cdot vma\_factor \cdot risk\_adj \\
&\qquad allow\_defer_{local} \leftarrow allow\_defer \wedge deferred\_count < max\_defer[class] \\
&\qquad (p_{next}\ge p_{thr} \wedge allow\_defer_{local}) ? DEFER : DUMP \\
&\quad \text{else if } parent\_ok \rightarrow PP\_HOLE\_PARENT \\
&\quad \text{else } DUMP \\
&\textbf{Else (final dump): if deferred } \rightarrow FORCE\_DUMP
\end{aligned}
$$

## 12. 脚本层执行流程（checkpoint_run_impl.sh）

### 12.1 阶段划分
- **Phase0 warm-up**：dirty-track 启动 → 等待 `WARMUP_SEC` → stop 生成 baseline → restart。
- **Predump loop**：
    - `PREDUMP_ITERS` 轮
    - 记录 `iteration_metrics.csv`
    - `PREDUMP_ZERO_RETRY` 避免偶发零页
- **Final dump**：强制回补 deferred
- **Optional**：
    - `EXTRA_FINAL_PREDUMP=1` 额外 predump
    - `SKIP_ACCURACY_ABORT_THRESHOLD` 低于阈值直接中止

### 12.2 传输时间驱动迭代间隔
- 默认 `PREDUMP_USE_TRANSFER_TIME=1`
- 估算：$transfer\_time_{ms} = \frac{bytes}{bandwidth\_bps} \cdot 1000$

## 13. 动态迭代停止策略（DM 专属）

### 13.1 Dirtymap/Deferred 信号（唯一判据）
当 `--use-dirty-map` 且 `PREDUMP_DM_ENABLE=1`：
- `predicted_accuracy = predicted_hit / predicted_total`（defer-only）
- `predicted_all_accuracy = (predicted_hit + predicted_hit_nondefer) / predicted_total_all`（综合指标，日志 `ObsidianPred2`）
- `def_growth = (def_total - prev_def_total) / prev_def_total`
- **收敛信号**：`pred_acc >= ACC_CONV` 且 `def_growth <= GROWTH_CONV`
- **发散信号**：`pred_acc <= ACC_DIV` 且 `def_growth >= GROWTH_DIV`

**决策规则**：
- 若 dirtymap 信号有效（`predicted_total >= MIN_PRED_TOTAL` 且 `prev_def_total > 0`），连续满足 **收敛** 或 **发散** 条件达到 `predump_stop_consec` 次即停止。
- 若 dirtymap 信号无效，则继续迭代直到 `PREDUMP_ITERS` 结束（不再使用 pages-only 作为“自适应”依据）。

### 13.2 策略参数（默认）
| policy | dm_min_pred_total | dm_acc_conv | dm_acc_div | dm_growth_conv | dm_growth_div | consec |
|---|---:|---:|---:|---:|---:|---:|
| aggressive | 128 | 60 | 50 | 0.15 | 0.25 | 2 |
| balanced | 256 | 65 | 55 | 0.12 | 0.25 | 2 |
| conservative | 512 | 70 | 55 | 0.08 | 0.25 | 2 |

可通过环境变量覆盖：
- `PREDUMP_DM_ENABLE`
- `PREDUMP_DM_MIN_PRED_TOTAL`
- `PREDUMP_DM_ACC_CONVERGE / PREDUMP_DM_ACC_DIVERGE`
- `PREDUMP_DM_DEF_GROWTH_CONVERGE / PREDUMP_DM_DEF_GROWTH_DIVERGE`

### 13.3 更强的 deferred_list 链分析（离线/可选）
## 14. 固定停止策略（pages-based，非 adaptive）

该策略不属于 adaptive，不依赖 DM，可用于所有模式（baseline/disabled/active）。默认启用：`PREDUMP_FIXED_STOP=1`。

固定阈值（balanced）：
- `gain_ratio < 0.02` 或 `cur <= 2048` → 视为低收益
- `cur > prev * 1.10` → 视为发散
- **plateau 稳定窗口**：最近 `window` 轮 pages 的 `max/min <= 1.15` 且 `min_pages >= 8192` → 视为稳定
- 连续次数 `consec=2`（plateau 使用独立的 `plateau_consec`，默认 1）

可调参数：
- `PREDUMP_FIXED_PLATEAU_WINDOW`
- `PREDUMP_FIXED_PLATEAU_RATIO`
- `PREDUMP_FIXED_PLATEAU_MIN_PAGES`
- `PREDUMP_FIXED_PLATEAU_CONSEC`

该稳定窗口用于处理 **pages 振荡但整体不再下降** 的 workload（如 InfluxDB / Elasticsearch），
避免在长传输间隔下反复 predump 造成时间浪费。

该策略仅用作 **固定早停**，与 DM-adaptive 相互独立。
如需更精细的收敛判定，可在离线分析中引入：
- **Jaccard overlap**：$|D_i \cap D_{i-1}| / |D_i \cup D_{i-1}|$
- **新页比例**：$|D_i - D_{i-1}| / |D_i|$
- **连续 defer 分布**：`count>=2` 的占比

这些指标可基于 `deferred_list.<pid>` 文件解析得到。

## 15. Telemetry 与指标

### 14.1 关键日志
- `ObsidianDecision`：单页决策（含 heat / p_next / p_thr / score 等）
- `ObsidianPred`：预测命中率
- `ObsidianPred2`：扩展准确率（nondefer / all）
- `ObsidianDef`：`deferred_total`
- `ObsidianDefFinal`：最终 deferred 正确性
- `ObsidianDEFER`：逐页 defer 记录（含 count、protected_until）
- `ObsidianEnforceQueue`：软预算回补统计

### 14.2 CSV 与摘要
- `iteration_metrics.csv`：每轮 pages / duration / pred_total / pred_hit / def_total 等
- `prediction_summary.txt`：累计预测准确率

## 16. 最新验证结果（2026-01-28）

### 15.1 Active vs Baseline：各迭代转储页
- Redis baseline: `34693;16645;8252;12134;6179;3419;2050;23262`, dump 11434
- Redis active: `31986;14568;11123;5801;3164;1957;1410;1156`, dump 1010

- InfluxDB baseline: `98017;33983;29843;31803;28804;30261;31319;30270`, dump 29821
- InfluxDB active: `67151;31820;56997;31388;30327;31803;33876;30271`, dump 29754

- Elasticsearch baseline: `119956;39577;30555;29008;33222;34128;31163;34073`, dump 31503
- Elasticsearch active: `112073;22013;20098;28971;30025;35380;32848;32223`, dump 29624

### 15.2 动态停止策略对比（total_pages / iteration_count）
- Redis: noadapt 77822/13, aggressive 70440/8, balanced 72098/9, conservative 76241/13
- InfluxDB: noadapt 427192/13, aggressive 403152/12, balanced 152096/4, conservative 437149/13
- Elasticsearch: noadapt 487604/13, aggressive 248524/6, balanced 488270/13, conservative 490098/13

## 17. 验收准则与注意事项

- 所有 deferred 必须在最终 dump 强制发送。
- predicted_accuracy（defer-only）长期稳定 ≥ 80%（经验目标），同时关注 `ObsidianPred2` 的 all-accuracy 趋势。
- 若 `predicted_total` 长期过低，需检查 deferred_list 是否过小或决策过于保守。

## 附录 A：legacy score 权重（基础分，dirty-map.c）

- `W0=-2.5`
- `W_HEAT=2.2`
- `W_TREND=1.3`
- `W_DEFER_CNT=1.0`
- `W_BURST=1.5`
- `W_DIRTY_FREQ=1.0`
- `PAT_DECL_PENALTY=1.6`
- `HIST_DECL_PENALTY=1.6`
- `SCORE_CLAMP=20`
- `CONF_K=2.0`
- `EMA_ALPHA=0.3`
- `DEFERRED_RISK_COEF=1.0`
- `CLASS_BIAS`: FREEZING=-1.0, COLD=-0.5, WARM=-0.2, HOT=0.6

在线校准默认参数（**默认启用**，可用 `CRIU_PREDICT_MODEL` 切换）：
- `online_bias=0.0`，`online_scale=1.0`
- `online_class_bias[*]=0.0`，`online_class_scale[*]=0.0`
- `CRIU_ONLINE_LR=0.05`，`CRIU_ONLINE_L2=0.0001`

## 附录 B：logistic skip 模型加载要点

- 仅在显式提供 `CRIU_SKIP_MODEL_FILE` 时启用。
- `best_threshold` 作为 `decision_p_threshold` 的保守下限。
- 可通过 `CRIU_SKIP_MODEL_MIN_THRESHOLD` 上调阈值以增强保守性。
