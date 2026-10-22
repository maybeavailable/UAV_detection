import { GatewayApi, errorMessage } from './api'
import {
  newMetrics,
  type Activity,
  type PlaybackGrant,
  type Source,
  type StreamState,
  type StreamView,
} from '../types'

type Report = (state: StreamView) => void
type Log = (level: Activity['level'], source: string, message: string) => void
const active: StreamState[] = ['connecting', 'connected', 'playing', 'disconnected', 'stopping']

export class VideoConnection {
  private view: StreamView = { state: 'idle', error: '', since: null, metrics: newMetrics() }
  private video: HTMLVideoElement | null = null
  private peer: RTCPeerConnection | null = null
  private grant: PlaybackGrant | null = null
  private mediaId = ''
  private version = 0
  private abort: AbortController | null = null
  private renewTimer = 0
  private statsTimer = 0
  private deadline = 0
  private disconnectTimer = 0
  private renewal: Promise<void> | null = null
  private stopping: Promise<void> = Promise.resolve()
  private last: {
    bytes: number
    frames: number
    received: number
    lost: number
    at: number
  } | null = null
  private lastFrameAt = 0

  constructor(
    readonly source: Source,
    private api: GatewayApi,
    private report: Report,
    private log: Log,
  ) {}
  attach(video: HTMLVideoElement): void {
    this.video = video
  }
  private update(patch: Partial<StreamView>): void {
    this.view = { ...this.view, ...patch }
    this.report(this.view)
  }
  private current(version: number): boolean {
    return this.version === version && this.peer !== null
  }

  async start(): Promise<void> {
    if (active.includes(this.view.state)) return
    const version = ++this.version
    this.update({ state: 'connecting', error: '', since: null, metrics: newMetrics() })
    this.log('info', this.source.name, '正在建立视频连接')
    await this.stopping
    if (version !== this.version) return
    const abort = new AbortController()
    this.abort = abort
    this.last = null
    try {
      const grant = await this.api.request<PlaybackGrant>(
        '/api/v1/playback',
        'POST',
        { stream_id: this.source.id, protocol: 'webrtc' },
        undefined,
        abort.signal,
      )
      if (version !== this.version) {
        await this.release(grant, '')
        return
      }
      this.grant = grant
      const peer = new RTCPeerConnection({ bundlePolicy: 'max-bundle' })
      this.peer = peer
      peer.addTransceiver('video', { direction: 'recvonly' })
      peer.ontrack = (event) => {
        if (!this.current(version) || !this.video) return
        this.video.srcObject = event.streams[0] ?? new MediaStream([event.track])
        void this.video.play().catch(() => {})
      }
      if (this.video)
        this.video.onplaying = () => {
          if (!this.current(version)) return
          window.clearTimeout(this.deadline)
          this.lastFrameAt = Date.now()
          if (!this.view.since) this.log('success', this.source.name, '实时画面已接入')
          this.update({ state: 'playing', since: this.view.since ?? Date.now() })
        }
      peer.onconnectionstatechange = () => {
        if (!this.current(version)) return
        if (peer.connectionState === 'failed') {
          void this.fail('视频连接失败，请检查网络后重试')
          return
        }
        if (peer.connectionState === 'disconnected') {
          this.update({ state: 'disconnected' })
          window.clearTimeout(this.disconnectTimer)
          this.disconnectTimer = window.setTimeout(() => {
            if (this.current(version) && peer.connectionState !== 'connected')
              void this.fail('网络连接已断开，请重新连接')
          }, 10000)
        } else if (peer.connectionState === 'connected') {
          window.clearTimeout(this.disconnectTimer)
          this.update({ state: this.view.since ? 'playing' : 'connected' })
        } else if (peer.connectionState === 'closed') void this.fail('设备已关闭视频连接')
      }
      const offer = await peer.createOffer()
      offer.sdp = this.withReceiverLimits(offer.sdp ?? '', grant)
      await peer.setLocalDescription(offer)
      await this.gather(peer, abort.signal)
      if (!this.current(version)) return
      const answer = await this.api.request<{ session_id: string; sdp: string }>(
        grant.url,
        'POST',
        { sdp: peer.localDescription?.sdp },
        grant.media_token,
        abort.signal,
      )
      if (!this.current(version)) {
        await this.release(grant, answer.session_id)
        return
      }
      this.mediaId = answer.session_id
      await peer.setRemoteDescription({ type: 'answer', sdp: answer.sdp })
      if (!this.current(version)) return
      this.deadline = window.setTimeout(() => {
        if (this.current(version) && !this.view.since)
          void this.fail('视频连接超时，请检查设备出图与网络')
      }, 20000)
      this.scheduleRenewal(version)
      this.statsTimer = window.setInterval(() => {
        void this.stats(peer, version)
      }, 1000)
    } catch (error) {
      if (version !== this.version) return
      await this.fail(errorMessage(error))
    }
  }
  private withReceiverLimits(sdp: string, grant: PlaybackGrant): string {
    const { max_fs: fs, max_mbps: mbps } = grant.parameters ?? {}
    if (!fs || !mbps) return sdp
    const h264 = new Set(
      [...sdp.matchAll(/^a=rtpmap:(\d+) H264\/90000/gim)].map((match) => match[1]),
    )
    return sdp
      .split('\r\n')
      .map((line) => {
        const match = line.match(/^a=fmtp:(\d+) /)
        return match && h264.has(match[1]!)
          ? `${line.replace(/;?max-fs=\d+/g, '').replace(/;?max-mbps=\d+/g, '')};max-fs=${fs};max-mbps=${mbps}`
          : line
      })
      .join('\r\n')
  }
  private gather(peer: RTCPeerConnection, signal: AbortSignal): Promise<void> {
    if (signal.aborted) return Promise.reject(new DOMException('Aborted', 'AbortError'))
    if (peer.iceGatheringState === 'complete') return Promise.resolve()
    return new Promise((resolve, reject) => {
      const cleanup = () => {
        window.clearTimeout(timer)
        signal.removeEventListener('abort', cancel)
        peer.removeEventListener('icegatheringstatechange', check)
      }
      const cancel = () => {
        cleanup()
        reject(new DOMException('Aborted', 'AbortError'))
      }
      const check = () => {
        if (peer.iceGatheringState === 'complete') {
          cleanup()
          resolve()
        }
      }
      const timer = window.setTimeout(() => {
        cleanup()
        reject(new Error('negotiation_timeout'))
      }, 7000)
      signal.addEventListener('abort', cancel, { once: true })
      peer.addEventListener('icegatheringstatechange', check)
      check()
    })
  }
  private scheduleRenewal(version: number): void {
    const ttl = this.grant?.expires_in_seconds ?? 120
    this.renewTimer = window.setTimeout(
      () => {
        if (!this.current(version) || !this.grant) return
        const grant = this.grant
        const renewal = this.api
          .request<Partial<PlaybackGrant>>(
            `/api/v1/playback/${encodeURIComponent(grant.session_id)}/renew`,
            'POST',
            {},
          )
          .then((next) => {
            // stop waits for renewal, including token rotation, before closing a media session.
            if (this.grant?.session_id === grant.session_id) this.grant = { ...grant, ...next }
            if (this.current(version)) this.scheduleRenewal(version)
          })
          .catch((error) => {
            if (this.current(version)) void this.fail(errorMessage(error))
          })
          .finally(() => {
            if (this.renewal === renewal) this.renewal = null
          })
        this.renewal = renewal
      },
      Math.max(500, ttl * 600),
    )
  }
  private async stats(peer: RTCPeerConnection, version: number): Promise<void> {
    try {
      const report = await peer.getStats()
      if (!this.current(version)) return
      let inbound: RTCInboundRtpStreamStats | null = null,
        rtt: number | null = null
      report.forEach((item) => {
        if (item.type === 'inbound-rtp' && (item.kind === 'video' || item.mediaType === 'video'))
          inbound = item as RTCInboundRtpStreamStats
        if (
          item.type === 'candidate-pair' &&
          item.state === 'succeeded' &&
          item.nominated &&
          Number.isFinite(item.currentRoundTripTime)
        )
          rtt = item.currentRoundTripTime * 1000
      })
      const input = inbound as RTCInboundRtpStreamStats | null
      if (!input) return
      const now = {
        bytes: input.bytesReceived ?? 0,
        frames: input.framesDecoded ?? 0,
        received: input.packetsReceived ?? 0,
        lost: input.packetsLost ?? 0,
        at: input.timestamp,
      }
      const previous = this.last,
        dt = previous ? (now.at - previous.at) / 1000 : 0
      const bitrate =
        previous && dt > 0 ? Math.max(0, ((now.bytes - previous.bytes) * 8) / dt) : null
      const fps =
        previous && dt > 0
          ? Math.max(0, (now.frames - previous.frames) / dt)
          : (input.framesPerSecond ?? null)
      const received = previous ? now.received - previous.received : 0,
        lost = previous ? Math.max(0, now.lost - previous.lost) : 0
      const loss = previous && received + lost > 0 ? (lost * 100) / (received + lost) : null
      const history =
        bitrate === null
          ? this.view.metrics.history
          : [...this.view.metrics.history, bitrate].slice(-30)
      if (previous && now.frames > previous.frames) this.lastFrameAt = Date.now()
      this.last = now
      const state =
        this.view.state === 'playing' && Date.now() - this.lastFrameAt > 3000
          ? 'connected'
          : now.frames && Date.now() - this.lastFrameAt < 3000
            ? 'playing'
            : this.view.state
      this.update({
        state,
        metrics: {
          bitrate,
          fps,
          rtt,
          loss,
          width: this.video?.videoWidth ?? 0,
          height: this.video?.videoHeight ?? 0,
          decoded: now.frames,
          history,
        },
      })
    } catch {
      /* A closing peer can reject getStats; lifecycle handlers own recovery. */
    }
  }
  private async release(grant: PlaybackGrant | null, id: string): Promise<void> {
    if (!grant) return
    if (id)
      await this.api
        .request(
          `/api/v1/webrtc/${encodeURIComponent(this.source.id)}/${encodeURIComponent(id)}`,
          'DELETE',
          undefined,
          grant.media_token,
        )
        .catch(() => {})
    await this.api
      .request(`/api/v1/playback/${encodeURIComponent(grant.session_id)}`, 'DELETE')
      .catch(() => {})
  }
  private async fail(message: string): Promise<void> {
    this.log('error', this.source.name, message)
    await this.stop('error', message)
  }
  async stop(endState: StreamState = 'idle', error = ''): Promise<void> {
    if (this.view.state === 'stopping') {
      await this.stopping
      return
    }
    const wasActive = active.includes(this.view.state),
      version = ++this.version
    this.abort?.abort()
    this.abort = null
    window.clearTimeout(this.renewTimer)
    window.clearInterval(this.statsTimer)
    window.clearTimeout(this.deadline)
    window.clearTimeout(this.disconnectTimer)
    const peer = this.peer
    this.peer = null
    if (peer) {
      peer.onconnectionstatechange = null
      peer.ontrack = null
      peer.close()
    }
    if (this.video) {
      this.video.onplaying = null
      this.video.srcObject = null
    }
    this.update({ state: 'stopping' })
    this.stopping = (async () => {
      await this.renewal
      const grant = this.grant,
        id = this.mediaId
      this.grant = null
      this.mediaId = ''
      await this.release(grant, id)
      if (version === this.version)
        this.update({ state: endState, error, since: null, metrics: newMetrics() })
      if (wasActive && endState === 'idle') this.log('info', this.source.name, '视频连接已关闭')
    })()
    await this.stopping
  }
  unload(): void {
    ++this.version
    this.abort?.abort()
    this.abort = null
    const peer = this.peer
    this.peer = null
    if (peer) {
      peer.onconnectionstatechange = null
      peer.ontrack = null
      peer.close()
    }
    if (this.video) {
      this.video.onplaying = null
      this.video.srcObject = null
    }
    window.clearTimeout(this.renewTimer)
    window.clearInterval(this.statsTimer)
    window.clearTimeout(this.deadline)
    window.clearTimeout(this.disconnectTimer)
    const grant = this.grant
    const id = this.mediaId
    this.grant = null
    this.mediaId = ''
    if (grant && id)
      this.api.unload(
        `/api/v1/webrtc/${encodeURIComponent(this.source.id)}/${encodeURIComponent(id)}`,
        grant.media_token,
      )
    if (grant) this.api.unload(`/api/v1/playback/${encodeURIComponent(grant.session_id)}`)
    // A page restored from the back/forward cache must show released connections.
    this.update({ state: 'idle', error: '', since: null, metrics: newMetrics() })
  }
}
