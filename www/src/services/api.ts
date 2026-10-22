import type { LoginSession } from '../types'

export class ApiError extends Error {
  constructor(
    public code: string,
    public status = 0,
  ) {
    super(code)
  }
}
export function errorMessage(error: unknown): string {
  const code = error instanceof Error ? error.message : String(error)
  const messages: Record<string, string> = {
    invalid_credentials: '接入凭据无效，请重新输入',
    unauthorized: '登录已失效，请重新登录',
    invalid_refresh_token: '登录已到期，请重新登录',
    login_rate_limited: '登录尝试过于频繁，请稍后再试',
    video_slot_unavailable: '该视频源正在被其他连接使用',
    stream_unavailable: '视频源暂不可用，请检查设备出图状态',
    media_unavailable: '视频服务暂不可用',
    media_token_invalid: '播放授权已失效，请重新连接',
    playback_expired: '播放授权已到期，请重新连接',
    device_disabled: '该设备已停用',
    stream_not_found: '设备尚未注册该视频源',
    h264_profile_or_resolution_not_supported: '浏览器不支持该视频源的解码规格',
    peer_connection_failed: '视频连接失败，请检查网络后重试',
    no_local_ice_candidates: '设备没有可用的媒体网络地址',
    frontend_signal_origin: '当前页面无法连接此媒体服务地址',
    timeout: '请求超时，请检查设备连接',
    network: '无法连接设备，请检查网络',
    negotiation_timeout: '视频连接超时，请重新连接',
  }
  return messages[code] || (code === 'Failed to fetch' ? messages.network : '连接未完成，请重试')
}

export class GatewayApi {
  private session: LoginSession | null = null
  private expiresAt = 0
  private version = 0
  private refreshing: Promise<void> | null = null
  onExpired: (() => void) | null = null

  private async send<T>(
    path: string,
    method: string,
    body: unknown,
    token: string | undefined,
    signal?: AbortSignal,
    keepalive = false,
  ): Promise<T> {
    const url = new URL(path, window.location.origin)
    if (url.origin !== window.location.origin) throw new ApiError('frontend_signal_origin')
    const abort = new AbortController()
    const cancel = () => abort.abort()
    signal?.addEventListener('abort', cancel, { once: true })
    if (signal?.aborted) abort.abort()
    const timer = window.setTimeout(() => abort.abort(), 12000)
    try {
      const response = await fetch(url, {
        method,
        body: body === undefined ? undefined : JSON.stringify(body),
        cache: 'no-store',
        signal: abort.signal,
        keepalive,
        headers: {
          ...(token ? { Authorization: `Bearer ${token}` } : {}),
          ...(body === undefined ? {} : { 'Content-Type': 'application/json' }),
        },
      })
      const result = await response.json()
      if (!response.ok || !result.ok)
        throw new ApiError(result.error?.code || `http_${response.status}`, response.status)
      return result.data as T
    } catch (error) {
      if (signal?.aborted) throw new DOMException('Aborted', 'AbortError')
      if (abort.signal.aborted) throw new ApiError('timeout')
      if (error instanceof ApiError) throw error
      throw new ApiError('network')
    } finally {
      window.clearTimeout(timer)
      signal?.removeEventListener('abort', cancel)
    }
  }
  async login(token: string): Promise<LoginSession> {
    const session = await this.send<LoginSession>(
      '/api/v1/auth/login',
      'POST',
      { token },
      undefined,
    )
    ++this.version
    this.session = session
    this.expiresAt = Date.now() + session.expires_in_seconds * 1000
    return session
  }
  private async access(): Promise<string> {
    if (!this.session) throw new ApiError('unauthorized', 401)
    if (Date.now() >= this.expiresAt - Math.min(5000, this.session.expires_in_seconds * 200)) {
      if (!this.refreshing) {
        const session = this.session,
          version = this.version
        this.refreshing = this.send<LoginSession>(
          '/api/v1/auth/refresh',
          'POST',
          { refresh_token: session.refresh_token },
          undefined,
        )
          .then((next) => {
            if (this.version === version) {
              this.session = next
              this.expiresAt = Date.now() + next.expires_in_seconds * 1000
            }
          })
          .catch((error) => {
            if (this.version === version) {
              this.clear()
              this.onExpired?.()
            }
            throw error
          })
          .finally(() => {
            this.refreshing = null
          })
      }
      await this.refreshing
    }
    if (!this.session) throw new ApiError('unauthorized', 401)
    return this.session.access_token
  }
  async request<T>(
    path: string,
    method = 'GET',
    body?: unknown,
    token?: string,
    signal?: AbortSignal,
  ): Promise<T> {
    try {
      return await this.send<T>(path, method, body, token ?? (await this.access()), signal)
    } catch (error) {
      if (
        token === undefined &&
        error instanceof ApiError &&
        error.status === 401 &&
        this.session
      ) {
        this.clear()
        this.onExpired?.()
      }
      throw error
    }
  }
  health(): Promise<{ running: boolean }> {
    return this.send('/healthz', 'GET', undefined, undefined)
  }
  async logout(): Promise<void> {
    try {
      if (this.session) await this.request('/api/v1/auth/logout', 'POST')
    } finally {
      this.clear()
    }
  }
  logoutOnExit(): void {
    const token = this.session?.access_token
    this.clear()
    if (token) {
      void fetch('/api/v1/auth/logout', {
        method: 'POST',
        headers: { Authorization: `Bearer ${token}` },
        keepalive: true,
      }).catch(() => {})
    }
  }
  clear(): void {
    ++this.version
    this.session = null
    this.expiresAt = 0
  }
  unload(path: string, mediaToken?: string): void {
    const token = mediaToken ?? this.session?.access_token
    if (!token) return
    // Page exit must not wait for a request or persist credentials.
    void fetch(path, {
      method: 'DELETE',
      headers: { Authorization: `Bearer ${token}` },
      keepalive: true,
    }).catch(() => {})
  }
}
