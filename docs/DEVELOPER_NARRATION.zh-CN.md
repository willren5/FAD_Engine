# FAD Engine 新开发者培训与代码导读文稿

对象：第一次接手 FAD Engine 的 C++ / DSP 开发者。配套材料为 [详细开发指南](DEVELOPMENT_GUIDE.zh-CN.md) 和 [最小示例](../examples/minimal/minimal_render.cpp)。

正文可以直接讲述；方括号中的内容是讲者操作提示，不需要念出来。每个单元结束都安排一个可以检查理解的问题。可分两次培训：第一场完成独立发声、架构和状态/资源；第二场完成 MIDI、FX、开发任务和验证。编译、试听、现场问答和练习按实际情况安排。

## 课前准备

[讲者准备]

- 打开本次隔离工作树、开发指南、`CMakeLists.txt` 和 `examples/minimal/minimal_render.cpp`。
- 使用自己独立的 build 目录，核对当前 compiler、generator 和 source root；避免占用其他任务的 build。
- 准备一个新的 WAV 输出名。已有 WAV 不覆盖，也不把用户原始工程拿来做练习。
- 如需真实 DAW，只在已核对的竖屏操作；本课程主体使用 headless 和最小 C++ 程序。
- 先说明当前许可为Community + Commercial Source-Available，以及完整 Noise80 的严格 deadline 仍为 FAIL。不要把课程描述成已经完成公共发布或全平台认证。

## 单元一 今天要学会什么

今天我们从一个最小的 C++ 程序开始，让 FAD Engine 真正输出一个音符。然后沿着这个音符读源码，看看参数、音符、资源和效果器分别由谁管理。最后做一个小任务，走完修改、验证和交接。

学完以后，你应该能回答四个问题：一个声音从哪里进入引擎；一次 render 读了哪些状态；改一个功能应当改哪个文件；怎样证明修改保持了既有合同。

先把工作范围说清楚。Engine 提供的是声音执行能力。登录、购买、ALT、WebView、音频设备和安装程序由外层产品负责。今天开发独立 DSP，不需要准备一个 FAD 账号，也不需要先走商业产品发行流程。

现在代码里同时存在两个名称空间。`fad::engine` 放 audio view、process spec 和一些基础接口，`fadmodel1::dsp` 放已经迁出的声音模块。这是当前真实的源码结构。接手时先按这个结构工作，不需要先做一轮全局改名。

[操作：打开 `CMakeLists.txt`，指出 `FadEngineCore`、`FAD::EngineCore`、C++20 和三个常用构建开关。]

这里最值得记住的是 target。自己的程序链接 `FAD::EngineCore`，公共 include 路径会随 target 传递。默认的 Core 不下载 VST® 3 SDK。你只有明确打开可选示例，才需要准备固定的官方 SDK。

[停顿：让学员指出 Core、renderer、VST3示例和 FAD Synth 产品分别在哪一层。]

## 单元二 先跑一个声音

[操作：按快速开始配置独立Core build，使用本地CMake/CTest，记录实际配置与结果，不逐条念构建日志。]

第一次构建时先看两件事：source 是不是我们刚打开的 `engine/`，build 是不是自己的目录。项目里旧测试二进制可能还在，程序能启动只能说明那个文件能运行，不能说明刚改的源码已经编译进去。

构建完成后，我们先用 WAV renderer。这里指定采样率、块大小、音符、力度、seed，以及四个归一化控制。输出名必须是新的；写文件失败时不要靠覆盖旧文件来判断结果。

[操作：运行 `FadEngineRender --help`，再用一个新路径渲染3秒WAV。指出 frames、finite、peak、RMS。需要试听时播放这一份明确编号的输出。]

这个例子已经经过真实 RenderEngine、VoiceCore、振荡器、滤波和包络。默认示例的 FX graph 是空的，Core 里仍有完整效果器能力。四个控制只是示例选出的界面，不代表引擎只有四个参数。

现在先不要急着调更多参数。检查输出是有限值、确实非静音，以及 note-off 后的 release 是否得到足够时间。一次有声音是起点，后面还要看时间线、恢复、资源变化和性能。

[提问：同一个 seed，换成一个已经运行很久的实例继续渲染，是否一定得到相同音频？]

不一定。还要匹配声音、初始状态、资源、reset 和事件时间线。随机生成器已经走了多少步，也会影响后续结果。做可重复比较时，我们会使用同一完整输入和清楚的初始边界。

## 单元三 读懂最小 C++ 程序

[操作：打开 `examples/minimal/minimal_render.cpp`，从对象声明读到 prepare。]

先看三个对象：bank、graph、engine。bank 保存发布的资源，graph 管理效果器，engine 借用前面两个对象。它们按这个顺序声明，所以退出作用域时 engine 先析构。

这个顺序有实际意义。Engine 结束时还要处理资源退役和自己的 worker。如果先销毁 bank 或 graph，Engine 就可能访问已经不存在的对象。把它们写成三个成员时，也要检查成员声明顺序，不能只看构造函数参数顺序。

下一段建立 snapshot。这里明确选择一个正弦振荡器，关闭不需要的声源和滤波，把随机相位、drift 等变量固定，再设包络。看字段时要注意单位：包络这里是秒，fine 是 cents，cutoff 是 Hz。

还有一个容易看错的地方：整体 snapshot 的构造函数会修改成员默认值。单独看 `OscillatorSnapshot` 的 level 初值，不能推断整个初始声音。找默认值时要同时读结构体和 `ParameterSnapshot.cpp`。

[操作：指出 `stageIsolatedOfflineInitialSnapshot` 和 `prepare` 前后的顺序。]

这个 stage 方法有很窄的使用条件：一个新的、由当前代码独占的离线实例，并且还没有 prepare。它让测试或 renderer 从明确初态开始。运行中的产品有自己的 publisher、restore epoch 和 Preview，这时不能拿 stage 方法当作快捷的载入预设入口。

准备完成后，我们创建左右声道数组和一个 plane pointer table。这个 view 不拥有音频内存。数组和 pointer table 都活着，这一次同步 render 才能安全使用它们。

[操作：逐行看循环中的 count、事件数组、块内 offset 和 `engine.render`；最后指出指标统计发生在 render 返回之后。]

例子中的打印、异常检查和指标汇总属于离线程序。接实时宿主时，文件写入、日志和复杂析构都不能搬进 callback。独立可运行与实时安全要分别检查。

[提问：把 `planes[]` 放在一个函数局部，然后返回一个引用它的 AudioView，安全吗？]

不安全。即使左右声道数据还在，pointer table 本身已经失效了。非拥有 view 的两个层次都要有有效寿命。

## 单元四 沿着声音找真正的 owner

[操作：展示指南第5章概念图，然后打开 `RenderEngine.h`、`VoiceCore.h` 和 `OscillatorBlock.h`。]

从宿主传进来的，是一份声音投影、资源引用和带时间的事件。RenderEngine 负责把这些输入协调成一个执行过程：接受完整声音、处理音符、固定本块资源，再调用 voice 和 graph。

VoiceCore 管理一个声部的实际状态。音符是否按住、踏板是否保持、这个 voice 是否活跃、它是什么 channel，都有明确 owner。现在 JUCE facade 不再维护另一份活动音符表。

振荡器、包络、滤波器和 smoother 也各自有运行状态。你不能只复制参数，就假定复制了正在进行的声音。一个 release 中的 voice 和一个刚启动的 voice，即使参数相同，状态也不同。

如果要修振荡器，先从 VoiceCore 怎么设置和调用 OscillatorBlock 往下看。如果修的是 routing、FX 切换或资源接受，就从 RenderEngine 或 graph 看。旧 `Source/DSP/` 目录有很多转发文件，先确认算法真正在哪个 owner 中，避免改到一个不执行算法的包装层。

[提问：新建一个编辑器时，能不能为了显示方便再维护一套“哪些音符正在按住”的声音状态？]

界面可以消费明确发布的显示数据；不能让另一套缓存成为声音 authority。否则 editor 关闭、pedal 和外部控制器一竞争，两个 owner 就可能给出不同结果。

## 单元五 状态为什么要完整发布

[操作：打开 `ParameterSnapshot.h`、`RenderParameterAccess.h`，说明下游publisher职责，再看 `setParameterSnapshotFromAudioThread`。]

想象用户载入一个声音，同时改变 oscillator mode、波表 slot 和 FX graph。如果音频线程先读到新 mode，却还拿着旧资源，结果可能是错误声音，也可能是无效指针。我们需要一次看到完整旧声音，或者完整新声音。

下游publisher 负责把 UI、host、import 和 Preview 的权威状态变成 snapshot。音频读取是有界尝试。读不到稳定候选时，继续旧声音；不能等 UI 写完。

Engine 接受候选也可能返回 false。比如对应资源 generation 还不能 pin，或者 FX graph 尚未准备好。这不是一个要求音频线程立即修好的异常。调用者保留当前完整声音，在后面的合法边界再尝试；候选已经被更新权威作废时就丢弃。

这里最容易出现的错误，是在 callback 里写一个 while，一直试到成功。这样只是把原来的有界接口变成无界等待。Core 的方法本身没有等待，不代表调用者包一层循环后仍然安全。

再区分文件与 runtime。`ParameterSnapshot` 是给 DSP 读的执行数据，不是磁盘上的 preset。产品 codec 还要处理版本、资产、program 和旧状态；示例插件则有自己非常小的版本化 state。不能把内存结构体整块写进文件，指望它长期恢复。

[操作：用一个“Preview还没Commit，host却保存工程”的情景提问。]

这时 host capture 应保存 Preview baseline 及合同允许的实时控制，而不是把临时试听意外保存成正式状态。host restore 如果已经获胜，旧 Preview 也不能再覆盖回来。DSP reset 没有权限随意取消 Preview。

## 单元六 资源最难的部分是退出

[操作：打开 `WavetableData.h` 的 frames/mip/metadata，再打开 `WavetableBank.h` 的 ownership 与 guard。]

波表和 Noise 样本不是一组 float 这么简单。它们可能包含 mips、PCM metadata、phase map、曲线和绘制历史。资源在控制侧准备，完成后作为不可变数据发布。

一旦正在渲染，就不能在原 vector 上 resize 或改内容。新版本先准备好，再由同一个 bank 发布新 generation。snapshot 要与它引用的这一代资产匹配。

音频侧为了避免频繁 shared-owner 操作，会使用有严格寿命的 raw pointer。它的安全来自一整套 handoff：先取得 resolution guard，找到 cohort，pin reader，再退出 resolution guard。旧资源要等所有可能的 reader 离开后，才由安全线程销毁。

所以这里“退出”比“发布一个新指针”更重要。把最后一个 shared_ptr 放到 callback 结束时析构，可能释放一大批内存。退役列表就是把这件事从实时路径移出去。

[操作：指出 bank 中 control mutex、RenderEngine 的 Noise publication 和 retirement capacity 检查。]

不要看到 Core 没有 JUCE，就把里面所有方法都当实时方法。`buildMipmap`、bank query、queue/commit 和 Noise 发布都有自己的控制侧条件。一个方法写了 noexcept，也可能拿锁。

如果资源发布容量满了，正确做法是受控失败并保留旧声音。不要临时扩容，也不要先改声音再通知外层“其实没有成功”。

[提问：为了新 EQ 曲线另建一个 bank，能不能让代码更独立？]

当前合同要求共用原 bank 和曲线/history owner。另建一套资源生命周期会让 snapshot、保存恢复和 graph 看见不同的代际。先接入已有事务，再讨论是否有确切理由改变架构。

## 单元七 时间线要落实到样本

[操作：回到最小示例的 note-off，画出全局帧和 block-local offset。]

假设 note-off 在第24000帧，当前块从23808开始，那块内位置就是192。我们只把这一块范围内的事件交进去。下一个块到来时，重新计算它自己的 offset。

block-size1、17或512，应该表达同一条时间线。若代码每个 block 重置 LFO、重新 seed，或者重复消费了边界事件，声音就会随宿主块大小变化。

另一个边界是通道编号。Core 的 ShortMessage 使用1到16，VST3事件使用0到15。demo adapter 只转换一次。不要在一个 helper 已经转换之后，调用者又减一次1。

同一点的事件也不能随意重排。先 note-off 再 note-on，可能与先 note-on 再 note-off 完全不同。保留现有 stable-order/source 合并规则。

输入容量也是声音合同。RenderEngine每条 MIDI lane 最多4096个事件。超量会请求既有 panic/fade，以免丢掉 note-off 后挂音。这个行为要让外层诊断看得见，但日志本身在安全线程处理。

[操作：打开 `DemoPlugin.cpp` 的 process，指出外层 callback scope 和内层 automation chunk。]

一个宿主 process 可以拆成多个片段，但仍是同一个 callback。不能每渲染一个片段，就重新打开 graph 接受边界。否则同一次真实 callback 的前半段和后半段可能拿到不同的结构状态。

## 单元八 看懂 FX 的三层结构

[操作：依次打开 BaseFxProcessorCore、FxCatalog/FxFactory、DynamicFxChainCore。]

一个效果器先有自己的处理合同：prepare分配什么，process读什么参数，reset怎样清理状态，bypass怎样过渡，latency和tail怎么报告。

catalog把类型和参数放在一起，factory负责创建对应processor，graph负责这些processor何时成为可听结构。新增一个类并让工厂返回它，只完成了其中一部分。

最常见的连续参数变化，应当复用已有processor和smoother。改变类型、routing或需要额外内存时，先准备完整结构。音频接受匹配snapshot之后，新graph才是可听authority。

切走一个reverb时，旧尾音不能因为新graph已构建就被立即删除。过渡期间latency也要覆盖old/new有效上界。给host发latency变化通知是控制侧的工作，不能随意放进process。

[提问：把某个FX参数从0改到1，只要更新一个float就够了吗？]

要看这个参数的语义。它可能是连续控制，也可能选择模式、改变lookahead或触发结构准备。先读catalog和processor里的真实路径，再决定更新节奏。

## 单元九 完成一次可审阅的开发任务

现在假设接到一个问题：“改变滤波cutoff时有点击声”。我们不要从“把所有东西平滑得更慢”开始。先固定一个能复现的声音和automation时间线。

[操作：在白板或任务文件里写 patch、seed、rate、block、MIDI、automation、reset和源码身份。]

沿着调用链找第一次发生突变的位置：是publisher撕裂、调制按block更新、filter系数切换、还是模式改变后状态没有正确衔接。问题在哪个owner，修复就尽量在那个owner完成。

改之前保存原始结果。改之后使用相同输入比较，不做音量归一化来遮住差异。涉及compatibility时，保留旧实现oracle；运行当前实现两遍只能证明它自己可重复。

如果修复只改变了算法，没有新增用户参数，就不需要顺带改整个UI和账号链路。如果确实增加了可保存、可自动化参数，才继续更新private contract、publisher、manifest、codec和对应bridge。

参数的旧ID、range、default、choice顺序和文件版本都属于兼容面。不能为了让新声音好听一点，就悄悄改变旧preset对同一个值的解释。想增加新声音，给它明确的新模式和迁移方案。

[练习讨论：让学员说出这次任务的“必须改”和“无需改”文件。按实际调用链纠正，不要求背整个仓库。]

最后交付的时候，不只说“修了，测试通过”。写清改了哪个owner，保持了什么合同，哪个精确二进制跑了哪些检查，还有哪些现象尚未验证。下一位开发者才能接着工作。

## 单元十 如何读验证结果

[操作：打开当前CTest列表，再看 `docs/VALIDATION.md` 对历史parity与Noise80范围的说明。]

先选能覆盖改动的现有检查。primitive变化看primitive差分，资源变化看publication/lifetime，graph变化看routing、latency和tail，preset或Preview变化再看它们自己的合同。

六个独立Core检查很适合开发起步，但每个检查都有范围。例如 dry renderer fixture 不能证明真实FX graph。看到程序打印PASS时，还要知道它实际执行了什么。

数值检查同时看finite、非静音、peak、max_abs、RMS和时间线。ASan/UBSan检查内存与未定义行为，TSan检查并发；它们的运行开销不能用来宣称正常性能。

这里还要保留一个当前事实：完整Noise80已经做了5轮配对，原版与候选的严格deadline都仍然FAIL。候选CPU中位数变化约+0.42%，峰值RSS约+7.99%，RSS复查标记仍在。工程提取完成，不会自动让这些失败变成通过。

以后做优化，保留全部轮次、相同预算和输入。不能只挑最快一轮，也不能把correctness-only当作完整性能测试。synthetic probe与真实硬件callback是两层证据。

宿主也是一样。示例在REAPER里发声、自动化和保存重开通过，只能证明这个具体模块和这些流程，不能据此宣布所有AU、Live、Windows或iOS都通过。

[提问：如果一个历史性能门本来就失败，新功能能不能直接跳过自己的数值、资源和RT验证？]

不能。把原有失败独立列出，同时检查这次是否引入新的问题。保持证据边界，才看得出某个回归来自哪里。

## 单元十一 上手练习

### 练习一 两个音符与跨块边界

[操作任务]

在最小消费者中使用一秒时间线：第0帧note60 on，第12000帧off；第18000帧note67 on，第36000帧off。保留同一seed和其他参数，分别使用block256和17渲染。

讲者台词：

这次只练adapter时间线，不改Core。先确认每个事件只进入一个block，offset合法，note-off不会丢。最后用相同时间线比较不同分块结果，并确认输出有限、非静音且尾部按包络释放。

[验收观察]

- 学员能解释全局帧与块内offset的转换。
- 两条时间线完全一致，没有重复note-on或遗漏note-off。
- 不把chunk数量变化解释为“可以重新seed”。

### 练习二 读一个参数的完整映射

[操作任务]

从 `DemoPatch.h` 的 Tone 开始，找出归一化控制怎样转成cutoffHz，再追踪到VoiceCore和FilterBlock。记录控制值domain、物理单位、默认值和automation进入点。

讲者台词：

先不要改公式。把真实路径画出来以后，再说“这个值从哪里来，在哪里变单位，在哪个边界被接受”。读清楚一条完整路径，比先加十个旋钮更有用。

[验收观察]

- 能区分demo四控制与产品canonical参数。
- 能指出normalized值与Hz的映射位置。
- 能说出修改默认值可能影响哪些旧状态和声音。

### 练习三 判断资源发布代码的线程

[操作任务]

给学员看 `WavetableData::buildMipmap`、bank `getPublishedSnapshot`、`RenderEngine::publishNoiseSampleSlot` 和 `setParameterSnapshotFromAudioThread`，让其分别标出允许线程、可能成本、返回失败行为和对象寿命。

讲者台词：

不要从函数名猜。读它真正调用了什么，看是否分配、拿锁、触发析构，是否有reader guard和retire capacity。最后指出哪一段可以在callback出现，哪一段应交给控制侧。

[验收观察]

- 不把noexcept等同于RT-safe。
- 不把shared_ptr等同于“不会在音频线程释放”。
- 能描述资源generation与snapshot必须一起考虑。

## 单元十二 常见问答

**可以只拿Engine做自己的插件吗？**

技术上可以从独立Core、renderer和固定SDK示例起步，不需要FAD私有UI和账户。使用或分发仍按当前许可和适用权利状态处理，不能把“可以构建”当作已经获准公开发布。

**为什么公共头里还能看到mutex、thread和shared_ptr？**

Core内部也有准备和退役职责。边界是具体方法和调用链，而不是头文件里是否出现某个标准库类型。控制侧可以做这些工作，音频侧只使用声明的有界接口。

**我能把JUCE AudioBuffer直接作为Engine公共类型吗？**

产品adapter可以把它转成借用view，公共Core保持自己的audio类型。这样同一算法才能由独立消费者、SDK示例和商业产品共同使用。

**能否在render里临时扩容，防止事件丢失？**

容量耗尽要走已声明的失败或panic策略。callback扩容把异常负载变成不可预测延迟，也可能破坏note-off与状态顺序。

**为什么设置snapshot返回false以后还能继续发声？**

它保留的是上一完整声音。外层需要正确安排后续尝试、处理更新权威和反馈，不能让音频线程等待半个新状态。

**怎么知道修改的是算法本体还是包装层？**

从真实render入口追调用链，再看CMake里实际编译与链接的源。旧Source文件可能只是alias或facade；Engine源码才是共享算法owner。

**做一个DSP实验需要跑登录、购买、到期和恢复吗？**

独立DSP实验不需要。涉及商业授权本身时才检查相关私有路径。算法开发使用独立consumer，不通过关闭产品gate来制造发声。

**什么时候可以优化一个atomic的memory order？**

先能解释完整handoff和退役协议，再建立相应并发验证与目标平台证据。只看一个load比另一个load快，不足以改变资源寿命协议。

**改源码后，旧的PASS还能继续用吗？**

先确认变化是否影响被测合同、binary是否来自当前source，以及hash是否一致。代码或构建条件变了，不能自动沿用旧PASS。文档只改了文字，也不需要机械重跑所有宿主。

## 结课与交接

接手Engine以后，先把一个声音和一条时间线固定下来。找到真实owner，再修改最接近问题的那一层。声音、参数、资源和graph按已有完整发布规则交接，音频线程始终保持有界。

每次交付都留下可以核对的输入、源码和二进制身份，以及实际检查结果。已有失败保留，没运行的范围写清。任务结束后清理自己的临时build和重复音频，把源码、必要交付和精简证据留下。

[讲者收尾：请每位学员用自己的话描述“输入 → snapshot/assets → render → 输出 → 验证”的完整链，并指出下一项任务的owning模块。随后打开指南第18章，用真实的小任务填写交接模板。]

## 讲者代码定位表

| 单元 | 现场打开 |
| --- | --- |
| 构建和第一次声音 | `CMakeLists.txt`、`examples/README.md`、`examples/headless/RenderWave.cpp`。 |
| 最小程序 | `examples/minimal/minimal_render.cpp`、`src/ParameterSnapshot.cpp`。 |
| 声音owner | `include/fad/engine/synth/RenderEngine.h`、`VoiceCore.h`、`src/VoiceCore.cpp`。 |
| 参数发布 | 下游publisher职责、`src/RenderEngine.cpp` 的两个setParameterSnapshot入口。 |
| 资源 | `include/fad/engine/assets/WavetableData.h`、`WavetableBank.h`、Noise publication/retirement。 |
| MIDI和automation | `include/fad/engine/midi/RenderMidiEvent.h`、`ShortMessage.h`、`examples/vst3/DemoPlugin.cpp`。 |
| FX | `include/fad/engine/fx/BaseFxProcessor.h`、`FxCatalog.h`、`FxFactory.h`、`DynamicFxChain.h`。 |
| 验证 | `tests/`、`docs/VALIDATION.md`。 |

文稿对应当前源码，实际检查和适用范围见 [VALIDATION.md](VALIDATION.md)。这里的课堂练习是待学员执行的任务，不冒充已经执行的测试结果。


---

<img src="../assets/steinberg/vst-compatible.png" width="160" alt="Official Steinberg VST Compatible logo">

<sub>VST is a registered trademark of Steinberg Media Technologies GmbH.</sub>
