export type SourceId = 'rgb.main' | 'rgb.sub' | 'thermal.main'
export type StreamState =
  'idle' | 'connecting' | 'connected' | 'playing' | 'disconnected' | 'stopping' | 'error'
export interface Source {
  id: SourceId
  name: string
  kind: string
  width: number
  height: number
  channel: string
}
export const sources: Source[] = [
  {
    id: 'rgb.main',
    name: '可见光主通道',
    kind: 'VISIBLE / MAIN',
    width: 2566,
    height: 1520,
    channel: '01',
  },
  {
    id: 'rgb.sub',
    name: '可见光子通道',
    kind: 'VISIBLE / SUB',
    width: 1920,
    height: 1080,
    channel: '02',
  },
  {
    id: 'thermal.main',
    name: '热成像',
    kind: 'THERMAL / INFRARED',
    width: 640,
    height: 512,
    channel: '03',
  },
]
export interface StreamMetrics {
  bitrate: number | null
  fps: number | null
  rtt: number | null
  loss: number | null
  width: number
  height: number
  decoded: number
  history: number[]
}
export interface StreamView {
  state: StreamState
  error: string
  since: number | null
  metrics: StreamMetrics
}
export interface Activity {
  id: number
  time: number
  level: 'info' | 'success' | 'error'
  source: string
  message: string
}
export interface LoginSession {
  session_id: string
  role: 'admin' | 'viewer'
  access_token: string
  refresh_token: string
  expires_in_seconds: number
  refresh_expires_in_seconds: number
}
export interface PlaybackGrant {
  session_id: string
  media_token: string
  expires_in_seconds: number
  url: string
  parameters: { max_fs?: number; max_mbps?: number; width?: number; height?: number; fps?: number }
}
export interface Device {
  id: string
  name: string
  type: string
  enabled: boolean
  status: Record<string, unknown>
}
export interface GatewayStatus {
  devices: Device[]
  requests: number
  pending_commands: number
  tls: boolean
}
export const newMetrics = (): StreamMetrics => ({
  bitrate: null,
  fps: null,
  rtt: null,
  loss: null,
  width: 0,
  height: 0,
  decoded: 0,
  history: [],
})
