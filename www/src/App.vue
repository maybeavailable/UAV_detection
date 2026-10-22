<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import {
  Activity as ActivityIcon,
  ArrowDownLeft,
  ArrowUpRight,
  Check,
  ChevronRight,
  CircleHelp,
  CircleUserRound,
  Clock3,
  Eye,
  Grid2X2,
  LayoutDashboard,
  List,
  LogOut,
  Monitor,
  Network,
  Play,
  Radio,
  RefreshCw,
  Server,
  ShieldCheck,
  Square,
  Trash2,
  Unplug,
  Wifi,
  X,
} from '@lucide/vue'
import VideoCard from './components/VideoCard.vue'
import AuthDialog from './components/AuthDialog.vue'
import { GatewayApi, errorMessage } from './services/api'
import { VideoConnection } from './services/video'
import {
  newMetrics,
  sources,
  type Activity,
  type Device,
  type GatewayStatus,
  type LoginSession,
  type SourceId,
  type StreamView,
} from './types'

const api = new GatewayApi()
const views = reactive<Record<string, StreamView>>(
  Object.fromEntries(
    sources.map((source) => [
      source.id,
      { state: 'idle', error: '', since: null, metrics: newMetrics() },
    ]),
  ),
)
const activities = ref<Activity[]>([]),
  activitySequence = ref(0)
const tab = ref<'monitor' | 'devices' | 'activity'>('monitor'),
  layout = ref<'focus' | 'grid'>('focus'),
  focused = ref<SourceId>('rgb.main')
const profile = ref<Pick<LoginSession, 'role' | 'session_id'> | null>(null)
const authOpen = ref(false),
  authBusy = ref(false),
  authError = ref(''),
  logoutBusy = ref(false)
const online = ref<boolean | null>(null),
  latency = ref<number | null>(null),
  lastChecked = ref<number | null>(null)
const gatewayStatus = ref<GatewayStatus | null>(null),
  registry = ref<string[] | null>(null)
const now = ref(new Date()),
  helpOpen = ref(false),
  notice = ref(''),
  errorOnly = ref(false)
const helpDialog = ref<HTMLDialogElement>()
watch(helpOpen, (open) => {
  if (open) helpDialog.value?.showModal()
  else helpDialog.value?.close()
})
const pending = ref<SourceId | 'all' | null>(null)
let pollTimer = 0,
  clockTimer = 0,
  noticeTimer = 0,
  pollBusy = false

function log(level: Activity['level'], source: string, message: string) {
  activities.value = [
    { id: ++activitySequence.value, time: Date.now(), level, source, message },
    ...activities.value,
  ].slice(0, 200)
}
const connections = Object.fromEntries(
  sources.map((source) => [
    source.id,
    new VideoConnection(
      source,
      api,
      (view) => {
        views[source.id] = view
      },
      log,
    ),
  ]),
) as Record<SourceId, VideoConnection>
const connectedCount = computed(
  () => sources.filter((source) => views[source.id]?.state === 'playing').length,
)
const anyActive = computed(() =>
  sources.some((source) =>
    ['connecting', 'connected', 'playing', 'disconnected'].includes(views[source.id]?.state ?? ''),
  ),
)
const anyStopping = computed(() => sources.some((source) => views[source.id]?.state === 'stopping'))
const bitrate = computed(() => {
  const rates = sources
    .map((source) => views[source.id]!.metrics.bitrate)
    .filter((rate): rate is number => rate !== null)
  return rates.length ? (rates.reduce((sum, rate) => sum + rate, 0) / 1000000).toFixed(2) : '—'
})
const errorCount = computed(
  () => activities.value.filter((event) => event.level === 'error').length,
)
const filteredActivities = computed(() =>
  errorOnly.value ? activities.value.filter((event) => event.level === 'error') : activities.value,
)
const devices = computed(() => gatewayStatus.value?.devices ?? [])
const title = computed(
  () => ({ monitor: '实时监看', devices: '设备状态', activity: '连接记录' })[tab.value],
)
const time = computed(() => now.value.toLocaleTimeString('zh-CN', { hour12: false }))
const day = computed(() =>
  now.value
    .toLocaleDateString('zh-CN', { year: 'numeric', month: '2-digit', day: '2-digit' })
    .replaceAll('/', '.'),
)
const host = window.location.host

function toast(message: string) {
  notice.value = message
  window.clearTimeout(noticeTimer)
  noticeTimer = window.setTimeout(() => {
    notice.value = ''
  }, 3500)
}
function unavailable(id: string) {
  return registry.value !== null && !registry.value.includes(id)
}
function focusSource(id: SourceId) {
  focused.value = id
  layout.value = 'focus'
}
function closeAuth() {
  authOpen.value = false
  pending.value = null
}
function requestLogin(intent: SourceId | 'all' | null = null) {
  pending.value = intent
  authError.value = ''
  authOpen.value = true
}
async function start(id: SourceId) {
  if (!profile.value) {
    requestLogin(id)
    return
  }
  await connections[id].start()
}
async function startAll() {
  if (!profile.value) {
    requestLogin('all')
    return
  }
  await Promise.all(
    sources
      .filter((source) => !unavailable(source.id))
      .map((source) => connections[source.id].start()),
  )
}
async function stopAll() {
  await Promise.all(sources.map((source) => connections[source.id].stop()))
}
async function refreshStatus() {
  if (!profile.value) return
  const [status, streams] = await Promise.all([
    api.request<GatewayStatus>('/api/v1/status'),
    api.request<{ id: string; protocols: string[]; backend_attached: boolean }[]>(
      '/api/v1/streams',
    ),
  ])
  gatewayStatus.value = status
  registry.value = streams
    .filter((stream) => stream.backend_attached && stream.protocols.includes('webrtc'))
    .map((stream) => stream.id)
}
async function poll() {
  if (pollBusy) return
  pollBusy = true
  try {
    const begin = performance.now(),
      health = await api.health()
    latency.value = Math.round(performance.now() - begin)
    online.value = health.running
    lastChecked.value = Date.now()
    if (profile.value) await refreshStatus().catch(() => {})
  } catch {
    online.value = false
    latency.value = null
  } finally {
    pollBusy = false
  }
}
async function login(token: string) {
  if (authBusy.value) return
  authBusy.value = true
  authError.value = ''
  try {
    const session = await api.login(token)
    profile.value = { role: session.role, session_id: session.session_id }
    await refreshStatus().catch(() => toast('登录成功，设备状态暂不可用'))
    authOpen.value = false
    log('success', '设备', '已登录设备')
    const intent = pending.value
    pending.value = null
    authBusy.value = false
    if (intent === 'all') await startAll()
    else if (intent) await start(intent)
  } catch (error) {
    authError.value = errorMessage(error)
  } finally {
    authBusy.value = false
  }
}
async function logout() {
  if (logoutBusy.value) return
  logoutBusy.value = true
  await stopAll()
  await api.logout().catch(() => {})
  profile.value = null
  gatewayStatus.value = null
  registry.value = null
  log('info', '设备', '已退出登录，视频连接已释放')
  logoutBusy.value = false
}
api.onExpired = () => {
  profile.value = null
  gatewayStatus.value = null
  registry.value = null
  void stopAll()
  log('error', '设备', '登录已到期，请重新登录')
  toast('登录已到期，请重新登录')
}
function deviceState(device: Device) {
  if (!device.enabled) return '已停用'
  const names: Record<string, string> = {
    capturing: '正常出图',
    waiting_frames: '等待出图',
    init_failed: '初始化失败',
    ready: '设备就绪',
    not_implemented: '待接入',
  }
  return names[String(device.status.state)] ?? '等待状态'
}
function activityTime(value: number) {
  return new Date(value).toLocaleTimeString('zh-CN', { hour12: false })
}
function closeOnExit() {
  for (const connection of Object.values(connections)) connection.unload()
  api.logoutOnExit()
  profile.value = null
  gatewayStatus.value = null
  registry.value = null
}
onMounted(() => {
  void poll()
  pollTimer = window.setInterval(() => {
    void poll()
  }, 5000)
  clockTimer = window.setInterval(() => {
    now.value = new Date()
  }, 1000)
  window.addEventListener('pagehide', closeOnExit)
})
onBeforeUnmount(() => {
  closeOnExit()
  window.clearInterval(pollTimer)
  window.clearInterval(clockTimer)
  window.clearTimeout(noticeTimer)
  window.removeEventListener('pagehide', closeOnExit)
})
</script>

<template>
  <div class="console-shell">
    <aside class="sidebar">
      <a class="brand" href="/" aria-label="UAV VISION 首页"
        ><img src="/icon.svg" width="39" height="39" alt="" />
        <div>
          <strong>UAV<span>VISION</span></strong
          ><small>光电感知控制台</small>
        </div></a
      >
      <div class="sidebar-section-label">工作空间 <span>WORKSPACE</span></div>
      <nav aria-label="主导航">
        <button
          :class="{ selected: tab === 'monitor' }"
          aria-label="实时监看"
          :aria-current="tab === 'monitor' ? 'page' : undefined"
          @click="tab = 'monitor'"
        >
          <LayoutDashboard :size="18" /><span>实时监看</span><span class="nav-count">03</span>
        </button>
        <button
          :class="{ selected: tab === 'devices' }"
          aria-label="设备状态"
          :aria-current="tab === 'devices' ? 'page' : undefined"
          @click="tab = 'devices'"
        >
          <Server :size="18" /><span>设备状态</span>
        </button>
        <button
          :class="{ selected: tab === 'activity' }"
          aria-label="连接记录"
          :aria-current="tab === 'activity' ? 'page' : undefined"
          @click="tab = 'activity'"
        >
          <List :size="18" /><span>连接记录</span
          ><span v-if="errorCount" class="nav-error">{{ errorCount }}</span>
        </button>
      </nav>
      <div class="sidebar-device">
        <div class="sidebar-device-symbol"><Radio :size="22" /></div>
        <span class="eyebrow">CONNECTED DEVICE</span><strong>光电监测终端</strong
        ><span class="sidebar-host">{{ host }}</span>
        <div class="sidebar-service">
          <span class="status-dot" :class="{ online: online }" />{{
            online === null ? '正在检测连接' : online ? '网关服务在线' : '网关服务离线'
          }}
        </div>
      </div>
      <div class="sidebar-bottom">
        <span class="build-indicator" />UAV DETECTION<span>v1.0</span>
      </div>
    </aside>
    <div class="main-shell">
      <header class="topbar">
        <div class="breadcrumb">
          <Monitor :size="16" /><span>控制台</span><ChevronRight :size="14" /><strong>{{
            title
          }}</strong>
        </div>
        <div class="topbar-actions">
          <span class="topbar-network" :class="{ offline: online === false }"
            ><Wifi :size="14" />{{
              online === null ? '检测中' : online ? '设备在线' : '连接中断'
            }}</span
          ><span class="topbar-divider" /><button
            v-if="!profile"
            class="login-button"
            data-testid="login-open"
            @click="requestLogin()"
          >
            <CircleUserRound :size="18" />登录设备<ChevronRight :size="14" />
          </button>
          <div v-else class="user-menu">
            <span class="user-avatar">{{ profile.role === 'admin' ? 'A' : 'V' }}</span
            ><span>{{ profile.role === 'admin' ? '管理员' : '观看用户' }}</span
            ><button
              class="icon-button"
              :disabled="logoutBusy"
              title="退出登录"
              aria-label="退出登录"
              @click="logout"
            >
              <LogOut :size="16" />
            </button>
          </div>
        </div>
      </header>
      <main>
        <section class="page-heading">
          <div>
            <div class="eyebrow">
              <span class="heading-mark" />{{
                tab === 'monitor'
                  ? 'LIVE MONITORING'
                  : tab === 'devices'
                    ? 'DEVICE STATUS'
                    : 'CONNECTION ACTIVITY'
              }}
            </div>
            <h1>{{ tab === 'monitor' ? '多视角实时监看' : title }}</h1>
            <p>
              {{
                tab === 'monitor'
                  ? '可见光与热成像协同，让每一个视角清晰可见。'
                  : tab === 'devices'
                    ? '查看设备接入状态与当前视频连接。'
                    : '本次页面会话的连接事件与异常记录。'
              }}
            </p>
          </div>
          <div class="page-clock">
            <Clock3 :size="14" /><strong>{{ time }}</strong
            ><span>{{ day }}</span>
          </div>
        </section>
        <section class="summary-grid" aria-label="运行概览">
          <div class="summary-card">
            <div class="summary-icon"><Eye :size="21" /></div>
            <div>
              <span>实时视频连接</span
              ><strong>{{ String(connectedCount).padStart(2, '0') }}<small>/ 03</small></strong>
            </div>
            <span class="summary-note">{{ connectedCount ? '正在接收画面' : '等待视频接入' }}</span>
          </div>
          <div class="summary-card">
            <div class="summary-icon blue"><ArrowDownLeft :size="22" /></div>
            <div>
              <span>总接收码率</span><strong>{{ bitrate }}<small>Mbps</small></strong>
            </div>
            <span class="summary-note">实时网络接收</span>
          </div>
          <div class="summary-card">
            <div class="summary-icon amber"><ActivityIcon :size="21" /></div>
            <div>
              <span>网关响应</span
              ><strong>{{ latency === null ? '—' : latency }}<small>ms</small></strong>
            </div>
            <span class="summary-note" :class="{ 'text-green': online }">{{
              online ? '连接正常' : online === false ? '服务暂不可达' : '正在检测'
            }}</span>
          </div>
        </section>

        <section v-show="tab === 'monitor'" class="monitor-view">
          <div class="section-toolbar">
            <div class="section-title">
              <span class="section-marker" />
              <h2>实时画面</h2>
              <span class="section-count">3 路视频源</span>
            </div>
            <div class="toolbar-actions">
              <div class="layout-switch" aria-label="画面布局">
                <button
                  :class="{ active: layout === 'focus' }"
                  title="主次视图"
                  aria-label="主次视图"
                  :aria-pressed="layout === 'focus'"
                  @click="layout = 'focus'"
                >
                  <LayoutDashboard :size="16" /></button
                ><button
                  :class="{ active: layout === 'grid' }"
                  title="等分视图"
                  aria-label="等分视图"
                  :aria-pressed="layout === 'grid'"
                  @click="layout = 'grid'"
                >
                  <Grid2X2 :size="16" />
                </button>
              </div>
              <button
                class="button button-subtle stop-all"
                data-testid="stop-all"
                :disabled="!anyActive || anyStopping"
                @click="stopAll"
              >
                <Square :size="13" />全部停止</button
              ><button
                class="button button-primary"
                data-testid="start-all"
                :disabled="connectedCount === 3 || anyStopping"
                @click="startAll"
              >
                <Play :size="14" />连接全部
              </button>
            </div>
          </div>
          <div class="video-grid" :class="`layout-${layout}`">
            <VideoCard
              v-for="source in sources"
              :key="source.id"
              :source="source"
              :view="views[source.id]!"
              :focused="focused === source.id"
              :unavailable="unavailable(source.id)"
              @ready="connections[source.id].attach($event)"
              @start="start(source.id)"
              @stop="connections[source.id].stop()"
              @focus="focusSource(source.id)"
              @notice="toast"
            />
          </div>
          <div class="monitor-footnote">
            <span><ShieldCheck :size="14" />每个视频源支持 1 路 WebRTC 同时观看</span
            ><button @click="helpOpen = true"><CircleHelp :size="14" />连接帮助</button>
          </div>
          <section class="source-overview">
            <div class="section-title">
              <h2>视频源概览</h2>
              <span class="section-count">{{
                profile ? '已连接设备目录' : '登录后读取设备状态'
              }}</span>
            </div>
            <div class="table-scroll">
              <table>
                <thead>
                  <tr>
                    <th>视频源</th>
                    <th>配置分辨率</th>
                    <th>实际帧率</th>
                    <th>接收码率</th>
                    <th>网络丢包</th>
                    <th>状态</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="source in sources" :key="source.id">
                    <td>
                      <span class="table-source-icon"><Monitor :size="14" /></span
                      ><strong>{{ source.name }}</strong
                      ><small class="source-id">{{ source.id }}</small>
                    </td>
                    <td class="mono">{{ source.width }} × {{ source.height }}</td>
                    <td class="mono">
                      {{
                        views[source.id]!.metrics.fps === null
                          ? '—'
                          : views[source.id]!.metrics.fps!.toFixed(1) + ' fps'
                      }}
                    </td>
                    <td class="mono">
                      {{
                        views[source.id]!.metrics.bitrate === null
                          ? '—'
                          : (views[source.id]!.metrics.bitrate! / 1000000).toFixed(2) + ' Mbps'
                      }}
                    </td>
                    <td class="mono">
                      {{
                        views[source.id]!.metrics.loss === null
                          ? '—'
                          : views[source.id]!.metrics.loss!.toFixed(1) + '%'
                      }}
                    </td>
                    <td>
                      <span
                        class="table-state"
                        :class="{ online: views[source.id]!.state === 'playing' }"
                        ><span class="status-dot" />{{
                          views[source.id]!.state === 'playing'
                            ? '正常接收'
                            : views[source.id]!.state === 'error'
                              ? '连接异常'
                              : '未接入'
                        }}</span
                      >
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
          </section>
        </section>

        <section v-show="tab === 'devices'" class="panel status-panel">
          <div class="section-toolbar">
            <div class="section-title">
              <h2>接入设备</h2>
              <span class="section-count">{{ devices.length }} 台</span>
            </div>
            <button
              class="button button-subtle"
              :disabled="!profile"
              @click="refreshStatus().catch(() => toast('暂时无法读取设备状态'))"
            >
              <RefreshCw :size="14" />刷新状态
            </button>
          </div>
          <div v-if="!profile" class="empty-state">
            <Server :size="36" />
            <h3>登录后查看设备状态</h3>
            <p>设备运行状态将从当前连接的网关读取。</p>
            <button class="button button-primary" @click="requestLogin()">
              登录设备<ArrowUpRight :size="14" />
            </button>
          </div>
          <div v-else-if="!devices.length" class="empty-state">
            <Network :size="32" />
            <h3>暂无注册设备</h3>
            <p>请检查设备目录和采集服务。</p>
          </div>
          <div v-else class="device-grid">
            <article v-for="device in devices" :key="device.id" class="device-card">
              <div class="device-card-top">
                <div class="summary-icon"><Monitor :size="23" /></div>
                <span
                  class="table-state"
                  :class="{
                    online:
                      device.enabled &&
                      ['capturing', 'ready'].includes(String(device.status.state)),
                  }"
                  ><span class="status-dot" />{{ deviceState(device) }}</span
                >
              </div>
              <h3>{{ device.name }}</h3>
              <span class="source-kind">{{ device.id }}</span>
              <dl>
                <div>
                  <dt>采集帧数</dt>
                  <dd>
                    {{
                      typeof device.status.frames === 'number'
                        ? device.status.frames.toLocaleString()
                        : '—'
                    }}
                  </dd>
                </div>
                <div>
                  <dt>采集错误</dt>
                  <dd>
                    {{ typeof device.status.errors === 'number' ? device.status.errors : '—' }}
                  </dd>
                </div>
                <div>
                  <dt>控制连接</dt>
                  <dd>
                    {{
                      typeof device.status.camera_control_connected === 'boolean'
                        ? device.status.camera_control_connected
                          ? '已连接'
                          : '未连接'
                        : '—'
                    }}
                  </dd>
                </div>
              </dl>
            </article>
          </div>
        </section>

        <section v-show="tab === 'activity'" class="panel activity-panel">
          <div class="section-toolbar">
            <div class="section-title">
              <h2>连接记录</h2>
              <span class="section-count">{{ activities.length }} 条</span>
            </div>
            <div class="toolbar-actions">
              <button
                class="button button-subtle"
                :class="{ 'is-filtered': errorOnly }"
                @click="errorOnly = !errorOnly"
              >
                {{ errorOnly ? '显示全部' : '仅看异常' }}</button
              ><button
                class="icon-button"
                title="清空记录"
                aria-label="清空连接记录"
                :disabled="!activities.length"
                @click="activities = []"
              >
                <Trash2 :size="17" />
              </button>
            </div>
          </div>
          <div v-if="!filteredActivities.length" class="empty-state">
            <List :size="34" />
            <h3>暂无连接记录</h3>
            <p>连接视频源后，相关事件会显示在这里。</p>
          </div>
          <ol v-else class="activity-list">
            <li v-for="event in filteredActivities" :key="event.id">
              <span class="event-mark" :class="event.level"
                ><Check v-if="event.level === 'success'" :size="14" /><Unplug
                  v-else-if="event.level === 'error'"
                  :size="14" /><Radio v-else :size="14"
              /></span>
              <div>
                <strong>{{ event.source }}</strong>
                <p>{{ event.message }}</p>
              </div>
              <time>{{ activityTime(event.time) }}</time>
            </li>
          </ol>
        </section>
        <footer class="page-footer">
          <span>UAV VISION <i />光电感知 · 实时连接</span
          ><span>{{
            lastChecked ? '状态更新于 ' + activityTime(lastChecked) : '正在连接设备'
          }}</span>
        </footer>
      </main>
    </div>
    <AuthDialog
      :open="authOpen"
      :busy="authBusy"
      :error="authError"
      @close="closeAuth"
      @login="login"
    />
    <div v-if="notice" class="toast" role="status"><Check :size="16" />{{ notice }}</div>
    <dialog
      ref="helpDialog"
      class="help-panel"
      aria-labelledby="help-title"
      @cancel.prevent="helpOpen = false"
    >
      <button class="icon-button dialog-close" aria-label="关闭连接帮助" @click="helpOpen = false">
        <X :size="19" />
      </button>
      <div class="dialog-symbol"><Network :size="24" /></div>
      <h2 id="help-title">连接与观看</h2>
      <p>登录设备后，点击「连接全部」即可同时查看三路视频，也可以单独连接所需视角。</p>
      <p>
        每个源默认支持一个 WebRTC 连接。提示视频源被占用时，请先关闭其他页面中该源的连接，再重试。
      </p>
      <p>
        截图和全屏操作位于画面右下角。码率、帧率和丢包由当前连接实时测量，尚未收到数据时显示「—」。
      </p>
      <button class="button button-primary" @click="helpOpen = false">知道了</button>
    </dialog>
  </div>
</template>
