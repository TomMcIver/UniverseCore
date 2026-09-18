import { PROTOCOL_VERSION, type ServerEnvelope } from './protocol'

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
  private hasReceivedInitialSnapshot = false

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
    this.hasReceivedInitialSnapshot = false
    this.onStateChange(0)

    this.socket.onopen = () => {
      this.reconnectAttempts = 0
      this.onStateChange(1)
      // Always re-anchor on a full snapshot after any (re)connect -- a
      // world_delta is never trusted as a valid continuation after a gap.
      this.send({ type: 'hello' })
      this.send({ type: 'request_snapshot' })
    }

    this.socket.onclose = () => {
      this.hasReceivedInitialSnapshot = false
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
        if (parsed.protocolVersion !== PROTOCOL_VERSION) {
          return
        }
        if (parsed.type === 'initial_snapshot') {
          this.hasReceivedInitialSnapshot = true
        } else if (parsed.type === 'world_delta' && !this.hasReceivedInitialSnapshot) {
          // Never apply a delta before we have a full snapshot to anchor on.
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

  /**
   * Sends a command to the server. Returns false without sending, and without
   * counting the message, when not connected -- callers must surface this
   * rather than assume the command was applied.
   */
  public send(payload: Record<string, unknown>): boolean {
    if (this.socket?.readyState !== WebSocket.OPEN) {
      return false
    }

    const serialized = JSON.stringify(payload)
    this.outgoingMessages += 1
    this.outgoingBytes += new Blob([serialized]).size
    this.socket.send(serialized)
    return true
  }
}
