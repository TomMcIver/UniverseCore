import type { ServerEnvelope } from './protocol'

export interface NetworkStats {
  incomingMessages: number
  outgoingMessages: number
  incomingBytes: number
  outgoingBytes: number
  reconnectAttempts: number
}

export class UniverseNetwork {
  private socket: WebSocket | null = null
  private readonly onMessage: (message: ServerEnvelope) => void
  private readonly onStateChange: (state: number) => void
  private url = ''
  private reconnectTimer: number | null = null
  private shouldReconnect = true
  private reconnectAttempts = 0
  private incomingMessages = 0
  private outgoingMessages = 0
  private incomingBytes = 0
  private outgoingBytes = 0

  public constructor(onMessage: (message: ServerEnvelope) => void, onStateChange: (state: number) => void) {
    this.onMessage = onMessage
    this.onStateChange = onStateChange
  }

  public connect(url: string): void {
    this.url = url
    this.shouldReconnect = true
    this.openSocket()
  }

  public disconnect(): void {
    this.shouldReconnect = false
    if (this.reconnectTimer !== null) {
      window.clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    this.socket?.close()
  }

  public stats(): NetworkStats {
    return {
      incomingMessages: this.incomingMessages,
      outgoingMessages: this.outgoingMessages,
      incomingBytes: this.incomingBytes,
      outgoingBytes: this.outgoingBytes,
      reconnectAttempts: this.reconnectAttempts,
    }
  }

  private openSocket(): void {
    this.socket = new WebSocket(this.url)
    this.onStateChange(0)

    this.socket.onopen = () => {
      this.reconnectAttempts = 0
      this.onStateChange(1)
      this.send({ type: 'hello' })
      this.send({ type: 'request_snapshot' })
    }

    this.socket.onclose = () => {
      this.onStateChange(2)
      this.scheduleReconnect()
    }

    this.socket.onerror = () => {
      this.onStateChange(2)
    }

    this.socket.onmessage = (event) => {
      const raw = String(event.data)
      this.incomingMessages += 1
      this.incomingBytes += new Blob([raw]).size
      try {
        const parsed = JSON.parse(raw) as ServerEnvelope
        if (parsed.protocolVersion !== 1) {
          return
        }
        this.onMessage(parsed)
      } catch {
        this.onStateChange(3)
      }
    }
  }

  private scheduleReconnect(): void {
    if (!this.shouldReconnect) {
      return
    }
    if (this.reconnectTimer !== null) {
      return
    }

    this.onStateChange(3)
    const delayMs = Math.min(5000, 500 + this.reconnectAttempts * 500)
    this.reconnectAttempts += 1
    this.reconnectTimer = window.setTimeout(() => {
      this.reconnectTimer = null
      this.openSocket()
    }, delayMs)
  }

  public send(payload: Record<string, unknown>): void {
    const serialized = JSON.stringify(payload)
    this.outgoingMessages += 1
    this.outgoingBytes += new Blob([serialized]).size

    if (this.socket?.readyState === WebSocket.OPEN) {
      this.socket.send(serialized)
    }
  }
}
