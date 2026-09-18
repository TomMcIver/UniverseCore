import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { UniverseNetwork } from './network'
import type { ServerEnvelope } from './protocol'

class MockWebSocket {
  public static readonly CONNECTING = 0
  public static readonly OPEN = 1
  public static readonly CLOSING = 2
  public static readonly CLOSED = 3
  public static instances: MockWebSocket[] = []

  public readyState = MockWebSocket.CONNECTING
  public sent: string[] = []
  public onopen: (() => void) | null = null
  public onclose: (() => void) | null = null
  public onerror: (() => void) | null = null
  public onmessage: ((event: { data: string }) => void) | null = null

  public constructor(public readonly url: string) {
    MockWebSocket.instances.push(this)
  }

  public send(data: string): void {
    this.sent.push(data)
  }

  public close(): void {
    this.readyState = MockWebSocket.CLOSED
    this.onclose?.()
  }

  public triggerOpen(): void {
    this.readyState = MockWebSocket.OPEN
    this.onopen?.()
  }

  public triggerMessage(payload: ServerEnvelope): void {
    this.onmessage?.({ data: JSON.stringify(payload) })
  }
}

function latestSocket(): MockWebSocket {
  const socket = MockWebSocket.instances.at(-1)
  if (socket === undefined) {
    throw new Error('no mock socket created')
  }
  return socket
}

function sentTypes(socket: MockWebSocket): string[] {
  return socket.sent.map((raw) => (JSON.parse(raw) as { type: string }).type)
}

describe('UniverseNetwork', () => {
  beforeEach(() => {
    MockWebSocket.instances = []
    vi.stubGlobal('WebSocket', MockWebSocket as unknown as typeof WebSocket)
    vi.useFakeTimers()
  })

  afterEach(() => {
    vi.useRealTimers()
    vi.unstubAllGlobals()
  })

  it('sends exactly one request_snapshot on every (re)connect, anchoring before anything else is trusted', () => {
    const network = new UniverseNetwork(
      () => {},
      () => {},
    )
    network.connect('ws://example.invalid')
    latestSocket().triggerOpen()

    const types = sentTypes(latestSocket())
    const requestSnapshotCount = types.filter((type) => type === 'request_snapshot').length
    expect(requestSnapshotCount).toBe(1)
    expect(types).toContain('request_snapshot')
  })

  it('does not apply a world_delta received before the post-connect initial_snapshot', () => {
    const received: ServerEnvelope[] = []
    const network = new UniverseNetwork(
      (message) => received.push(message),
      () => {},
    )
    network.connect('ws://example.invalid')
    latestSocket().triggerOpen()

    latestSocket().triggerMessage({ protocolVersion: 2, type: 'world_delta', payload: {} })
    expect(received).toHaveLength(0)

    latestSocket().triggerMessage({ protocolVersion: 2, type: 'initial_snapshot', payload: {} })
    expect(received).toHaveLength(1)

    latestSocket().triggerMessage({ protocolVersion: 2, type: 'world_delta', payload: {} })
    expect(received).toHaveLength(2)
  })

  it('send() does not call the underlying socket and reports failure when not connected', () => {
    const network = new UniverseNetwork(
      () => {},
      () => {},
    )
    network.connect('ws://example.invalid')
    // Socket created but never opened -- readyState stays CONNECTING.

    const result = network.send({ type: 'pause' })

    expect(result).toBe(false)
    expect(latestSocket().sent).toHaveLength(0)
  })

  it('send() succeeds once connected', () => {
    const network = new UniverseNetwork(
      () => {},
      () => {},
    )
    network.connect('ws://example.invalid')
    latestSocket().triggerOpen()
    latestSocket().sent = []

    const result = network.send({ type: 'pause' })

    expect(result).toBe(true)
    expect(sentTypes(latestSocket())).toEqual(['pause'])
  })

  it('fires ConnectionState transitions in order across a disconnect and reconnect', () => {
    const states: number[] = []
    const network = new UniverseNetwork(
      () => {},
      (state) => states.push(state),
    )

    network.connect('ws://example.invalid')
    latestSocket().triggerOpen()
    latestSocket().close()
    vi.runOnlyPendingTimers()
    latestSocket().triggerOpen()

    expect(states).toEqual([0, 1, 2, 3, 0, 1])
  })
})
