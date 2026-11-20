# 平行视界（应用页面左右分栏）

许多国内应用在安装包的`assets/easygo.json`中自带华为平板的分栏规则（`magicwindow`部分）。平行视界读取这些规则，转换为AOSP的Activity嵌入（Activity Embedding）规则，横屏时把应用的页面左右分栏显示。用户在"设置 → 应用 → 平行视界（实验性）"中按应用开启，默认全部关闭。

前提：内屏使用大屏默认的`ignoreOrientationRequest`（[#158](https://github.com/vahiru/gaokun-android/pull/158)）。否则只支持竖屏的应用会把整块屏幕转为竖屏，无法分栏。

## 组成

| 部分 | 位置 | 作用 |
|---|---|---|
| 兼容变更 | `patches/android/frameworks-base/0006`（`ActivityInfo`、`ActivityThread`、`WindowExtensionsHelper`） | 新增`OVERRIDE_ENABLE_PARALLEL_VIEW`（3374131364，可覆盖，默认关闭）。开启的应用在`handleBindApplication`末尾初始化窗口管理扩展库，不依赖虚拟手柄的aconfig标志`virtual_gamepad_override` |
| 规则转换 | `patches/android/frameworks-base/0007`（`libs/WindowManager/Jetpack`的`ParallelViewRules`） | 读取`easygo.json`并生成嵌入规则；处理两类妨碍扩展库识别页面的应用行为 |
| 设置页面 | `device/huawei/gaokun3/parts`的`ParallelViewSettingsActivity` | 列出带规则的应用，按应用设置兼容覆盖 |

### 扩展库的加载

AOSP只为虚拟手柄提供了"系统替应用载入嵌入规则"的路径：PMS在扫描时给开启了手柄变更的应用追加扩展库，`ActivityThread`在绑定应用时初始化嵌入。PMS只在扫描时追加，开关切换后要重启系统才生效。

0006不改PMS：应用的类加载器中找不到`WindowExtensionsProvider`时，`WindowExtensionsHelper`改用zygote为扩展库缓存的类加载器（`ApplicationLoaders.getCachedNonBootclasspathSystemLib`，后三个参数必须为null）。声明了扩展库的应用拿到的也是这个类加载器，所以每个进程中只有一份扩展库实例。切换开关后只需重启该应用。

### 规则对应关系

只读取第一个`magicwindow`部分；`excludeDevice`含`PAD`时不生成规则。声明了`PROPERTY_ACTIVITY_EMBEDDING_SPLITS_ENABLED`的应用自己使用分栏，与虚拟手柄相同，不载入规则。应用把`PROPERTY_ACTIVITY_EMBEDDING_ALLOW_SYSTEM_OVERRIDE`设为false时（微信、钉钉）仍然载入，因为规则来自应用自身。

| easygo | 嵌入规则 |
|---|---|
| `mode` 1（自定义）的`activityPairs` `{from, to}` | 每对一条`SplitPairRule`；`*`匹配任意页面；从左侧打开新页面时替换右侧，左侧结束时右侧随之结束 |
| `mode` 0（购物） | 一条通配`SplitPairRule`：新页面都在右侧打开，原右侧页面推到左侧。此模式下`activityPairs`不生效。桌面入口页面（通常是启动后即结束的闪屏页）不参与，除非它是默认分栏的主页面 |
| `defaultDualActivities`，以及只有`mainPage`/`relatedPage`的`activityPairs` | `SplitPlaceholderRule`：主页面打开时在右侧显示`relatedPage`；`mainPages`可用逗号分隔多个主页面 |
| `transActivities` | 不参与分栏 |
| `Activities`中`defaultFullScreen`为true的页面 | `ActivityRule`，始终铺满任务 |
| `UX.windowsRatio`中`PAD`的`ratio`（"左\|右"像素） | 左侧所占比例，限于0.2–0.8，否则对半 |
| `UX.isDraggable`为true | 可拖动的分隔条，左侧可在25%–75%之间调整；未声明时视为false，不设分隔条 |
| `UX.supportDraggingToFullScreen`含`PAD`或为true | 分隔条可拖到边缘，使一侧铺满任务。此键没有公开文档，含义按名称推断 |

其他条件：

- 只在横屏、且父窗口宽度不小于600 dp时分栏。
- 通配规则不让浮动主题（对话框样式）的页面进入右侧。
- 没有写明组件的Intent先解析出目标页面再匹配。
- 应用自带的规则未覆盖的过渡页，在`ParallelViewRules`中按应用补充（目前只有哔哩哔哩的半透明横幅页）。

### 应用行为的兼容处理

以下处理只对开启了平行视界的应用生效：

- **生命周期回调**：部分应用（淘宝；知乎、哔哩哔哩也重写了该方法）重写`Application.registerActivityLifecycleCallbacks()`，把回调包装成只转发七个旧式方法的类，`onActivityPreCreated()`因此不会到达，规则永远不会被匹配。应用重写了该方法时，扩展库把回调直接加入`Application`自身的列表。
- **启动监视器**：部分应用在启动后更换`Instrumentation`（淘宝），或让新的`Instrumentation`把启动转交给原实例（微信）。扩展库的启动监视器因此看不到页面启动，新页面不会进入右侧。扩展库在初始化时、以及每个页面创建前`Instrumentation`发生变化时，把监视器加入当前`Instrumentation`及其字段中保存的`Instrumentation`的列表。

两者都通过反射读写`Application`与`Instrumentation`的私有字段。扩展库位于`/system_ext/framework`，属于平台域，不受隐藏API限制。

### 全屏播放

分栏中的页面处于多窗口模式，部分应用在此模式下禁用全屏播放：哔哩哔哩的全屏按钮只检查`Activity.isInMultiWindowMode()`，为true时提示"分屏模式下暂不支持全屏播放"。0007对载入了平行视界规则的应用这样处理：

- **多窗口状态**：页面位于分栏中、且任务本身不处于多窗口模式时，报告为不处于多窗口模式；其他情况按实际报告（系统分屏、自由窗口、画中画）。`ActivityThread`只在页面创建时以及进入、退出多窗口模式时设置这个状态，所以每次布局后、以及任务变化时都重新检查。容器的窗口模式本身仍是多窗口，窗口管理依赖它确定容器的边界与可见性。
- **展开**：右侧容器的页面请求横屏时，用`SplitController.updateSplitAttributes()`把分栏改为`ExpandContainersSplitType`并去掉分隔条：该页面铺满任务，左侧页面保留在下面，视频上没有把手。该页面不再请求横屏时，恢复原来的分栏属性。右侧容器中的页面按窗口管理决定屏幕方向的规则判断：从最上层往下，第一个明确请求了方向的页面做决定；请求“跟随下层”（behind）的页面，以及没有请求方向的半透明或浮动样式页面，让给下层；没有请求方向的不透明页面则终止判断。其他进程的页面不参与判断。因此播放页上叠加的页面不会在播放页仍可见时恢复分栏，主题为半透明的播放页也能触发展开。右侧为非粘性占位页的分栏不展开，因为展开会关闭占位页及其上的页面。
- **请求的方向**：页面最近一次传给`setRequestedOrientation()`的值（`Activity.mLastRequestedOrientation`）；没有时向系统查询一次，得到清单中声明的方向，或重建前的实例请求的方向。
- 布局监听按页面实例登记，页面销毁时移除，因为页面重建时可能沿用原来的窗口。`SplitController`构造时把自己交给`ParallelViewRules`。

不处理的情况：左侧页面请求横屏（AOSP展开左侧时会结束右侧页面）；不切换为横屏的全屏（只隐藏系统栏，或竖屏视频）；按窗口大小而不是`isInMultiWindowMode()`判断分屏的应用。应用仍会收到`onMultiWindowModeChanged()`回调。

### 设置页面

- 列表：带启动器图标的非系统应用中，`easygo.json`有`magicwindow`部分、未排除平板、且未声明`SPLITS_ENABLED`的应用。页面只判断规则是否存在，规则的解析只在扩展库中进行。
- 开关：`CompatChanges.putPackageOverrides()`同时开启`OVERRIDE_ENABLE_PARALLEL_VIEW`与AOSP自带的`FORCE_RESIZE_APP`，关闭时`removePackageOverrides()`移除两者。覆盖保存在兼容框架中，重启后保留。
  - 需要`FORCE_RESIZE_APP`的原因：`WindowOrganizerController.createTaskFragment()`拒绝以不可调整大小的页面为起点创建分栏容器。手柄变更自带这一效果，新变更没有。已在右侧的页面由系统视为可调整大小，不受影响。
- 覆盖只在应用进程启动时读取，因此切换后用`killUid`结束该应用的进程。这与`am compat`的做法相同，不会让应用进入停止状态，推送与闹钟不受影响。

## 验证

- 规则载入：`adb logcat -s ParallelView`，应用启动时输出模式、比例、分隔条与规则数量。
- 覆盖状态：`adb shell dumpsys platform_compat | grep -E 'PARALLEL_VIEW|FORCE_RESIZE_APP'`。
- 分栏状态：`adb shell dumpsys activity containers`中，该应用任务下的两个`TaskFragment`为`MULTI-WINDOW`，边界分别为左右两侧。
- 回归项见[`release-checklist.md`](release-checklist.md)中的"平行视界设置"。

## 限制

- 全屏播放只处理右侧页面请求横屏的情况，见上文“全屏播放”。
- 微信只在识别到厂商平行视界、厂商平板白名单或折叠屏时，才把聊天单独打开为页面；首页的聊天显示在主界面内，不分栏。
- 应用请求权限时，系统权限对话框可能被右侧页面遮挡一部分。
- 自定义模式（mode 1）的配对规则指向应用中已不存在的页面时，相应页面不会分栏。购物模式不读取配对规则，不受此影响（例如淘宝的配对规则仍指向旧的页面名称）。
