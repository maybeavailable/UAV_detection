<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import {
  Camera,
  Expand,
  Maximize2,
  Play,
  Radio,
  RotateCw,
  ScanLine,
  Square,
  Waves,
} from '@lucide/vue'
import type { Source, StreamState, StreamView } from '../types'

const props = defineProps<{
  source: Source
  view: StreamView
  focused: boolean
  unavailable: boolean
}>()
const emit = defineEmits<{
  ready: [video: HTMLVideoElement]
  start: []
  stop: []
  focus: []
  notice: [message: string]
}>()
const video = ref<HTMLVideoElement>(),
  card = ref<HTMLElement>()
onMounted(() => {
  if (video.value) emit('ready', video.value)
})
const labels: Record<StreamState, string> = {
  idle: '未连接',
  connecting: '连接中',
  connected: '等待画面',
  playing: '实时',
  disconnected: '网络恢复中',
  stopping: '正在关闭',
  error: '连接异常',
}
const active = computed(() =>
  ['connecting', 'connected', 'playing', 'disconnected'].includes(props.view.state),
)
const canCapture = computed(() => props.view.metrics.decoded > 0)
const bitrate = computed(() =>
  props.view.metrics.bitrate === null ? '—' : (props.view.metrics.bitrate / 1000000).toFixed(2),
)
const spark = computed(() => {
  const values = props.view.metrics.history,
    max = Math.max(...values, 1)
  return values
    .map(
      (value, index) =>
        `${(index * 90) / Math.max(values.length - 1, 1)},${26 - (value / max) * 22}`,
    )
    .join(' ')
})
async function fullscreen() {
  try {
    if (document.fullscreenElement) await document.exitFullscreen()
    else await card.value?.requestFullscreen()
  } catch {
    emit('notice', '当前浏览器无法进入全屏')
  }
}
function snapshot() {
  const element = video.value
  if (!element?.videoWidth || element.readyState < 2) return
  const canvas = document.createElement('canvas')
  canvas.width = element.videoWidth
  canvas.height = element.videoHeight
  canvas.getContext('2d')?.drawImage(element, 0, 0)
  canvas.toBlob((blob) => {
    if (!blob) return
    const url = URL.createObjectURL(blob),
      link = document.createElement('a')
    link.href = url
    link.download = `${props.source.id}-${new Date().toISOString().replace(/[:.]/g, '-')}.png`
    link.click()
    window.setTimeout(() => URL.revokeObjectURL(url), 1000)
    emit('notice', `${props.source.name}截图已保存`)
  }, 'image/png')
}
</script>

<template>
  <article
    ref="card"
    class="video-card"
    :class="{
      'is-focused': focused,
      'is-playing': view.state === 'playing',
      'is-error': view.state === 'error',
    }"
    :data-source="source.id"
  >
    <header class="video-card-head">
      <div class="source-heading">
        <span class="channel-index">{{ source.channel }}</span>
        <div>
          <h3>{{ source.name }}</h3>
          <span class="source-kind">{{ source.kind }}</span>
        </div>
      </div>
      <div class="card-head-actions">
        <span class="stream-badge" :data-state="view.state"
          ><span class="status-dot" />{{ labels[view.state] }}</span
        >
        <button
          class="icon-button focus-button"
          type="button"
          :aria-label="`突出显示${source.name}`"
          title="突出显示"
          @click="emit('focus')"
        >
          <Maximize2 :size="15" />
        </button>
      </div>
    </header>
    <div class="video-stage">
      <video ref="video" autoplay playsinline muted :aria-label="source.name" />
      <div class="stage-label">
        <span class="tiny-square" />{{ source.width }} × {{ source.height
        }}<span class="stage-label-codec">H.264</span>
      </div>
      <span v-if="view.state === 'playing'" class="live-label"><span />LIVE</span>
      <div
        v-if="view.state !== 'playing'"
        class="video-placeholder"
        :class="{ buffered: view.metrics.decoded > 0 }"
      >
        <div class="reticle" :class="{ searching: view.state === 'connecting' }">
          <ScanLine v-if="source.id !== 'thermal.main'" :size="29" :stroke-width="1.2" /><Waves
            v-else
            :size="30"
            :stroke-width="1.2"
          />
        </div>
        <strong>{{
          view.state === 'error'
            ? '暂时无法接入画面'
            : view.state === 'idle'
              ? '等待视频接入'
              : labels[view.state]
        }}</strong>
        <p>
          {{
            view.error ||
            (unavailable
              ? '当前设备尚未提供此视频源'
              : view.state === 'idle'
                ? '连接后查看实时画面'
                : '正在与设备建立实时连接')
          }}
        </p>
        <button
          v-if="['idle', 'error'].includes(view.state)"
          class="stage-play"
          :disabled="unavailable"
          type="button"
          @click="emit('start')"
        >
          <RotateCw v-if="view.state === 'error'" :size="14" /><Play v-else :size="14" />{{
            view.state === 'error' ? '重新连接' : '接入画面'
          }}
        </button>
      </div>
      <div class="stage-tools">
        <button
          class="stage-tool"
          :disabled="!canCapture"
          :aria-label="`保存${source.name}截图`"
          title="保存截图"
          @click="snapshot"
        >
          <Camera :size="17" /></button
        ><button
          class="stage-tool"
          :aria-label="`全屏查看${source.name}`"
          title="全屏"
          @click="fullscreen"
        >
          <Expand :size="17" />
        </button>
      </div>
    </div>
    <footer class="video-card-foot">
      <div class="video-metrics">
        <span
          ><i class="metric-led" :class="{ on: view.state === 'playing' }" />{{ bitrate
          }}<small>Mbps</small></span
        ><span
          >{{ view.metrics.fps === null ? '—' : Math.round(view.metrics.fps)
          }}<small>FPS</small></span
        ><span class="rtt-metric"
          >{{ view.metrics.rtt === null ? '—' : Math.round(view.metrics.rtt)
          }}<small>ms</small></span
        >
      </div>
      <svg
        v-if="view.metrics.history.length > 1"
        class="bitrate-spark"
        viewBox="0 0 92 30"
        aria-label="最近接收码率"
      >
        <polyline :points="spark" fill="none" stroke="currentColor" stroke-width="1.5" />
      </svg>
      <button
        v-if="active"
        class="card-stop"
        type="button"
        :aria-label="`停止${source.name}`"
        @click="emit('stop')"
      >
        <Square :size="12" />停止
      </button>
      <button
        v-else
        class="card-connect"
        type="button"
        :disabled="view.state === 'stopping' || unavailable"
        :aria-label="`连接${source.name}`"
        @click="emit('start')"
      >
        <Radio :size="13" />连接
      </button>
    </footer>
  </article>
</template>
